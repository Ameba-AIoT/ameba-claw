"""
WebSocket session isolation tests.

WHY these tests exist (gap in test_session_rest_api.py):
  The 46 REST-only tests verify /api/session file state (chat_map) but never
  touch the WebSocket path.  The real bug — "send in session A, response lands
  in session B" — lives entirely in the WS message-routing chain:
    ws_on_message  → alias taken from {alias:X} field
    cap_im_local_send_for_alias → alias taken from resp->source_message_id
  Neither of these is reachable from pure REST calls.

WHAT is tested here:
  1. WS sync gives back only messages for the requested alias (not cross-alias).
  2. A WS message sent with alias=B while server current=A is stored under B,
     NOT under A.  This is the exact invariant that was broken:
       old: cap_im_local_send used get_current() → stored under wrong alias
       new: cap_im_local_send_for_alias() uses the alias from the request
  3. No duplicate snapshot on WS connect (ws_on_open no longer auto-pushes).

WHAT cannot be tested here (and why):
  The LLM response half of the bug (on_response → source_message_id → alias)
  requires the LLM to actually process a request, which is too slow and
  non-deterministic for an automated test suite.  The invariant is covered by
  the fix in ameba_claw_main.c; the WS storage half is verified below.
"""
import json
import sys
import os
import time
import threading
import unittest

import requests
import websocket

sys.path.insert(0, os.path.dirname(__file__))
from config import BOARD_BASE_URL, HTTP_TIMEOUT

BASE = BOARD_BASE_URL
SESSION_URL = BASE + "/api/session"
WS_URL = "ws://127.0.0.1/ws/chat"

# How long to wait for a WS frame before giving up
WS_RECV_TIMEOUT = 5.0


# ---------------------------------------------------------------------------
# Filename helpers (mirrors cap_session_mgr.c, same as test_session_mgr.py)
# ---------------------------------------------------------------------------

def _djb2(s: str) -> int:
    h = 5381
    for c in s.encode("utf-8"):
        h = ((h << 5) + h + c) & 0xFFFFFFFF
    return h


def _sanitize(s: str, max_len: int = 32) -> str:
    return "".join(c if (c.isalnum() or c in "._-") else "_" for c in s)[:max_len]


def chat_map_path(channel: str, chat_id: str) -> str:
    key = f"{channel}:{chat_id}"
    return f"/session/chat_map/s_{_sanitize(key)}_{_djb2(key):08x}.json"


# ---------------------------------------------------------------------------
# REST helpers
# ---------------------------------------------------------------------------

def files_delete(path: str):
    requests.delete(BASE + "/api/files", params={"path": path}, timeout=HTTP_TIMEOUT)


def delete_local_chat_map():
    """Ensure the board is responsive, then delete the chat_map."""
    _board_ready()
    try:
        files_delete(chat_map_path("local", "local"))
    except Exception:
        pass  # file may not exist; transient errors are OK after _board_ready()
    time.sleep(0.3)


def session_post(body: dict):
    return requests.post(SESSION_URL, json=body, timeout=HTTP_TIMEOUT)


def session_delete(alias: str):
    return requests.delete(SESSION_URL, params={"alias": alias}, timeout=HTTP_TIMEOUT)


# ---------------------------------------------------------------------------
# WebSocket helper
# ---------------------------------------------------------------------------

class _WsClient:
    """Thin synchronous WebSocket wrapper for testing."""

    def __init__(self):
        self._ws = websocket.WebSocket()
        self._ws.settimeout(WS_RECV_TIMEOUT)
        self._ws.connect(WS_URL)

    def send(self, obj: dict):
        self._ws.send(json.dumps(obj))

    def recv_until(self, predicate, timeout: float = WS_RECV_TIMEOUT):
        """Receive frames until predicate(frame_dict) returns True or timeout."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                raw = self._ws.recv()
                d = json.loads(raw)
                if predicate(d):
                    return d
            except websocket.WebSocketTimeoutException:
                break
            except Exception:
                break
        return None

    def recv_snapshot(self, alias: str, timeout: float = WS_RECV_TIMEOUT):
        """Wait for a {type:'snapshot', alias:alias} frame."""
        return self.recv_until(
            lambda d: d.get("type") == "snapshot" and d.get("alias") == alias,
            timeout=timeout,
        )

    def close(self):
        try:
            self._ws.close()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Availability guard
# ---------------------------------------------------------------------------

def _ws_available() -> bool:
    try:
        c = _WsClient()
        c.close()
        return True
    except Exception:
        return False


_no_ws = not _ws_available()
_skip = unittest.skipIf(_no_ws, "WebSocket /ws/chat not reachable")

# SO_SNDTIMEO on WS sockets is 1s. After a Python WS client closes, the board's
# ws_handshake_and_serve recv-loop must observe the EOF and call ws_unregister to
# free the slot. Until then, any broadcast attempt to the dying socket takes up to
# 1s. A 4s pause between test classes covers 1s×4 connections worst case.
_WS_TEARDOWN_SETTLE = 4.0

# ---------------------------------------------------------------------------
# LLM drain helper
#
# Tests that send user messages via WS trigger asynchronous LLM inference.
# The LLM response arrives as a broadcast to ALL connected WS clients.
# If a subsequent test class opens a WS connection before the LLM finishes,
# the stale response frame lands in that test's frame collection window and
# breaks alias-equality assertions.
#
# Fix: call _drain_ws_frames() in tearDownClass of any test that sends user
# messages, so the LLM response is consumed before the next class runs.
# ---------------------------------------------------------------------------

_LLM_QUIET_SECS = 8.0   # silence window: no new frame for this long → LLM done
_LLM_DRAIN_MAX  = 35.0  # absolute ceiling to avoid hanging on a stuck LLM


def _drain_ws_frames(quiet_secs: float = _LLM_QUIET_SECS,
                     max_secs: float = _LLM_DRAIN_MAX) -> None:
    """Open a WS connection and discard all incoming frames until quiet.

    Blocks until no frame arrives for `quiet_secs`, or `max_secs` elapses.
    Silently returns on any connection error (board may already be idle).
    """
    try:
        ws = websocket.WebSocket()
        ws.settimeout(quiet_secs)
        ws.connect(WS_URL)
        deadline = time.time() + max_secs
        while time.time() < deadline:
            try:
                ws.recv()            # discard — we only want silence
            except websocket.WebSocketTimeoutException:
                break                # quiet_secs of no frames → done
            except Exception:
                break
        try:
            ws.close()
        except Exception:
            pass
    except Exception:
        pass


def _board_ready(timeout: float = 10.0) -> bool:
    """Wait until a POST /api/session completes in under 2s.
    Mirrors the same helper in test_session_rest_api.py — confirms no HTTP
    connection tasks are backed up on post-response WS broadcasts."""
    deadline = time.time() + timeout
    probe = "_probe_ws_ready_"
    while time.time() < deadline:
        try:
            t0 = time.time()
            r = requests.post(SESSION_URL, json={"action": "new", "alias": probe},
                              timeout=3)
            elapsed = time.time() - t0
            try:
                requests.post(SESSION_URL,
                              json={"action": "resume", "alias": "default"},
                              timeout=2)
                requests.delete(SESSION_URL, params={"alias": probe}, timeout=2)
            except Exception:
                pass
            if elapsed < 2.0 and r.status_code in (200, 409):
                return True
        except Exception:
            pass
        time.sleep(0.5)
    return False


# ---------------------------------------------------------------------------
# Test: no duplicate snapshot on connect
#
# Old behaviour: ws_on_open fired push_session_history immediately, THEN
# the client sent {type:'sync'} which triggered a second push — two
# clearChat()+render cycles, flicker, wasted bandwidth.
# New behaviour: ws_on_open is a no-op; only the client's sync triggers a push.
# ---------------------------------------------------------------------------

@_skip
class TestWsNoDuplicateSnapshotOnConnect(unittest.TestCase):
    """ws_on_open must NOT send an unsolicited snapshot; only sync does."""

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        # Connect without sending any sync; collect all frames for 1s
        ws = websocket.WebSocket()
        ws.settimeout(1.0)
        ws.connect(WS_URL)
        cls.unsolicited = []
        deadline = time.time() + 1.2
        while time.time() < deadline:
            try:
                raw = ws.recv()
                d = json.loads(raw)
                if d.get("type") == "snapshot":
                    cls.unsolicited.append(d)
            except websocket.WebSocketTimeoutException:
                break
            except Exception:
                break
        ws.close()

    @classmethod
    def tearDownClass(cls):
        time.sleep(_WS_TEARDOWN_SETTLE)

    def test_no_snapshot_without_sync(self):
        """Server must not push a snapshot unless the client requests one."""
        self.assertEqual(
            len(self.unsolicited), 0,
            f"Got {len(self.unsolicited)} unsolicited snapshot(s): {self.unsolicited}",
        )


# ---------------------------------------------------------------------------
# Test: WS sync returns only messages for the requested alias
# ---------------------------------------------------------------------------

@_skip
class TestWsSyncFiltersAlias(unittest.TestCase):
    """sync for alias X returns only messages tagged with alias X."""

    ALIAS_A = "ws_iso_a"
    ALIAS_B = "ws_iso_b"
    MSG_A = "message_for_alias_a_only"
    MSG_B = "message_for_alias_b_only"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS_A})
        time.sleep(0.1)
        session_post({"action": "new", "alias": cls.ALIAS_B})
        time.sleep(0.1)

        ws = _WsClient()
        try:
            # Send a message for alias A
            session_post({"action": "resume", "alias": cls.ALIAS_A})
            time.sleep(0.1)
            ws.send({"text": cls.MSG_A, "alias": cls.ALIAS_A})
            time.sleep(0.3)

            # Send a message for alias B
            ws.send({"text": cls.MSG_B, "alias": cls.ALIAS_B})
            time.sleep(0.3)

            # Request sync for A
            ws.send({"type": "sync", "alias": cls.ALIAS_A})
            cls.snap_a = ws.recv_snapshot(cls.ALIAS_A)

            # Request sync for B
            ws.send({"type": "sync", "alias": cls.ALIAS_B})
            cls.snap_b = ws.recv_snapshot(cls.ALIAS_B)
        finally:
            ws.close()

    @classmethod
    def tearDownClass(cls):
        # Purge ring buffer for all aliases used by this class so that any
        # still-running LLM response cannot pollute subsequent tests' sync.
        # resume→clear is the only path that calls cap_im_local_clear_alias().
        for alias in (cls.ALIAS_A, cls.ALIAS_B):
            try:
                session_post({"action": "resume", "alias": alias})
                session_post({"action": "clear"})
            except Exception:
                pass
        time.sleep(_WS_TEARDOWN_SETTLE)

    def test_snap_a_has_msg_a(self):
        self.assertIsNotNone(self.snap_a, "No snapshot received for alias A")
        texts = [m.get("text", "") for m in self.snap_a.get("messages", [])]
        self.assertIn(self.MSG_A, texts, f"MSG_A not in A's snapshot: {texts}")

    def test_snap_a_no_msg_b(self):
        self.assertIsNotNone(self.snap_a)
        texts = [m.get("text", "") for m in self.snap_a.get("messages", [])]
        self.assertNotIn(self.MSG_B, texts,
                         "MSG_B leaked into session A snapshot (cross-session bleed)")

    def test_snap_b_has_msg_b(self):
        self.assertIsNotNone(self.snap_b, "No snapshot received for alias B")
        texts = [m.get("text", "") for m in self.snap_b.get("messages", [])]
        self.assertIn(self.MSG_B, texts, f"MSG_B not in B's snapshot: {texts}")

    def test_snap_b_no_msg_a(self):
        self.assertIsNotNone(self.snap_b)
        texts = [m.get("text", "") for m in self.snap_b.get("messages", [])]
        self.assertNotIn(self.MSG_A, texts,
                         "MSG_A leaked into session B snapshot (cross-session bleed)")


# ---------------------------------------------------------------------------
# Test: THE KEY BUG SCENARIO
#   Server current session = A.
#   Client sends WS message with alias = B.
#   Message must be stored under B, NOT under A.
#
# This is the storage half of the "sent in A, received in B" bug.
# Old code: cap_im_local_send used get_current() → stored under A.
# New code: ws_on_message stores user msg under the alias from the WS frame,
#           and cap_im_local_send_for_alias uses source_message_id for response.
# ---------------------------------------------------------------------------

@_skip
class TestWsMessageStoredUnderCorrectAlias(unittest.TestCase):
    """
    Core regression: WS message with alias=B is stored under B even when
    the server's current session is A (different session).
    """

    ALIAS_A = "ws_cur_a"   # server current
    ALIAS_B = "ws_cur_b"   # message target
    MSG_B = "regression_msg_must_be_in_b_not_a"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS_A})
        time.sleep(0.1)
        session_post({"action": "new", "alias": cls.ALIAS_B})
        time.sleep(0.1)
        # Make server current = A
        session_post({"action": "resume", "alias": cls.ALIAS_A})
        time.sleep(0.2)

        ws = _WsClient()
        try:
            # Confirm server current is A
            get_r = requests.get(SESSION_URL, timeout=HTTP_TIMEOUT)
            cls.server_current_before = get_r.json().get("current")

            # Send message for alias B while server current = A
            ws.send({"text": cls.MSG_B, "alias": cls.ALIAS_B})
            time.sleep(0.5)

            # Sync for B — must contain MSG_B
            ws.send({"type": "sync", "alias": cls.ALIAS_B})
            cls.snap_b = ws.recv_snapshot(cls.ALIAS_B)

            # Sync for A — must NOT contain MSG_B
            ws.send({"type": "sync", "alias": cls.ALIAS_A})
            cls.snap_a = ws.recv_snapshot(cls.ALIAS_A)
        finally:
            ws.close()

    def test_server_current_was_a(self):
        """Pre-condition: server current must be A when message was sent."""
        self.assertEqual(self.server_current_before, self.ALIAS_A,
                         "Pre-condition failed: server current was not A")

    def test_msg_stored_in_b(self):
        """MSG_B must appear in session B's snapshot."""
        self.assertIsNotNone(self.snap_b, "No snapshot for alias B")
        texts = [m.get("text", "") for m in self.snap_b.get("messages", [])]
        self.assertIn(self.MSG_B, texts,
                      f"Message not in session B — stored in wrong session. B texts: {texts}")

    def test_msg_not_in_a(self):
        """MSG_B must NOT appear in session A's snapshot (no cross-session bleed)."""
        self.assertIsNotNone(self.snap_a, "No snapshot for alias A")
        texts = [m.get("text", "") for m in self.snap_a.get("messages", [])]
        self.assertNotIn(self.MSG_B, texts,
                         f"Message leaked into session A — this is the regression. A texts: {texts}")

    @classmethod
    def tearDownClass(cls):
        # Purge ring buffer for the alias used so any still-running LLM
        # response cannot pollute subsequent tests' sync results.
        for alias in (cls.ALIAS_A, cls.ALIAS_B):
            try:
                session_post({"action": "resume", "alias": alias})
                session_post({"action": "clear"})
            except Exception:
                pass
        time.sleep(_WS_TEARDOWN_SETTLE)


# ---------------------------------------------------------------------------
# Test: sync response includes alias field matching the request
# ---------------------------------------------------------------------------

@_skip
class TestWsSyncResponseAlias(unittest.TestCase):
    """snapshot frame must carry alias matching the sync request."""

    ALIAS = "ws_alias_echo"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.2)

        ws = _WsClient()
        try:
            ws.send({"type": "sync", "alias": cls.ALIAS})
            cls.snap = ws.recv_snapshot(cls.ALIAS)
        finally:
            ws.close()

    @classmethod
    def tearDownClass(cls):
        time.sleep(_WS_TEARDOWN_SETTLE)

    def test_snapshot_alias_matches_request(self):
        self.assertIsNotNone(self.snap, "No snapshot received")
        self.assertEqual(self.snap.get("alias"), self.ALIAS,
                         f"snapshot.alias mismatch: {self.snap}")

    def test_snapshot_messages_is_list(self):
        self.assertIsNotNone(self.snap)
        self.assertIsInstance(self.snap.get("messages"), list)


# ---------------------------------------------------------------------------
# Test: incremental push — real-time message delivery without full re-fetch
# ---------------------------------------------------------------------------

@_skip
class TestWsIncrementalPush(unittest.TestCase):
    """
    After the initial sync, new messages arrive as incremental push frames
    (role/text/alias fields, no 'type' field), not as a new full snapshot.
    The frontend must never need to re-request a full history to stay current.
    """

    ALIAS = "ws_incr"
    MSG = "incremental_push_test_msg"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.2)

        ws = _WsClient()
        cls.incremental_frames = []
        cls.got_snapshot = False
        try:
            # Initial sync
            ws.send({"type": "sync", "alias": cls.ALIAS})
            snap = ws.recv_snapshot(cls.ALIAS, timeout=3)
            cls.got_snapshot = snap is not None

            # A second client sends a message for this alias
            ws2 = _WsClient()
            try:
                ws2.send({"text": cls.MSG, "alias": cls.ALIAS})
                time.sleep(0.5)
            finally:
                ws2.close()

            # Collect frames — should be an incremental push, not another snapshot
            deadline = time.time() + 2.0
            while time.time() < deadline:
                try:
                    raw = ws._ws.recv()
                    d = json.loads(raw)
                    cls.incremental_frames.append(d)
                except websocket.WebSocketTimeoutException:
                    break
                except Exception:
                    break
        finally:
            ws.close()

    def test_got_initial_snapshot(self):
        self.assertTrue(self.got_snapshot, "Initial sync snapshot not received")

    def test_received_incremental_frame(self):
        """At least one push frame must arrive after the peer sends a message."""
        self.assertGreater(len(self.incremental_frames), 0,
                           "No incremental frames received after peer sent message")

    def test_push_is_not_full_snapshot(self):
        """Incremental frames must not be full snapshot re-loads."""
        snapshots = [f for f in self.incremental_frames if f.get("type") == "snapshot"]
        self.assertEqual(len(snapshots), 0,
                         f"Got full snapshot re-load instead of incremental push: {snapshots}")

    def test_push_frames_carry_alias(self):
        """Incremental push frames for THIS session must carry a non-empty alias.

        The server broadcasts ALL assistant frames to all connected WS clients;
        alias filtering is the client's responsibility.  A frame belonging to a
        different session (foreign alias) may legitimately arrive in this
        connection's window — skip it.  Only assert on frames that are either
        untagged or belong to this alias, to catch bugs where cap_im_local omits
        the alias field on responses for our session.
        """
        for f in self.incremental_frames:
            if f.get("role") == "assistant":
                alias = f.get("alias", "")
                if alias and alias != self.ALIAS:
                    continue    # foreign-session broadcast — not our concern
                self.assertTrue(alias,
                                f"Assistant frame missing alias field: {f}")

    def test_user_msg_reachable_via_sync_not_push(self):
        """User messages are not broadcast to other WS clients (by design —
        the sender adds the bubble locally).  Verify the message IS accessible
        via sync instead, so no data is lost."""
        ws = _WsClient()
        try:
            ws.send({"type": "sync", "alias": self.ALIAS})
            snap = ws.recv_snapshot(self.ALIAS, timeout=3)
        finally:
            ws.close()
        self.assertIsNotNone(snap, "Sync snapshot not received")
        texts = [m.get("text", "") for m in snap.get("messages", [])]
        self.assertIn(self.MSG, texts,
                      f"User message not retrievable via sync. Got: {texts}")

    @classmethod
    def tearDownClass(cls):
        try:
            session_post({"action": "resume", "alias": cls.ALIAS})
            session_post({"action": "clear"})
        except Exception:
            pass
        time.sleep(_WS_TEARDOWN_SETTLE)


# ---------------------------------------------------------------------------
# Test: action=clear must purge the in-memory WS ring buffer
#
# Bug: claw_memory_clear_session() deleted the disk file but left
#      cap_im_local's ring buffer intact.  A fresh WS sync after clear
#      still returned the old messages because push_session_history()
#      reads from s_msgs[], not from the disk file.
# Fix: cap_webui clear handler now calls cap_im_local_clear_alias() to
#      zero out ring-buffer entries for the cleared alias, so the next
#      sync response contains an empty messages array.
#
# Sequence:
#   1. Create session, send WS message → enters ring buffer + disk.
#   2. Verify sync returns the message (pre-condition).
#   3. POST action=clear.
#   4. New WS connection, sync for same alias → messages must be empty.
# ---------------------------------------------------------------------------

@_skip
class TestWsClearContextPurgesRingBuffer(unittest.TestCase):
    """After action=clear, WS sync must return an empty messages list.

    KEY ORDERING CONSTRAINT:
    The LLM processes user messages asynchronously — its response arrives
    AFTER the WS send returns.  If we call action=clear before the LLM
    has finished, the response is pushed to the ring buffer AFTER the clear
    and the post-clear sync would see those new messages.

    Fix: call _drain_ws_frames() between the user message and the clear so
    the ring buffer contains the complete conversation (user + assistant)
    when clear is invoked, and nothing is in-flight at clear time.
    """

    ALIAS = "ws_clr_rb"
    # Short, unambiguous string that the LLM will echo back without tool calls.
    MSG   = "ws_clear_rb_test_7391"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.1)

        # Step 1: push a user message — it enters the ring buffer immediately
        ws = _WsClient()
        try:
            ws.send({"text": cls.MSG, "alias": cls.ALIAS})
            time.sleep(0.3)
            # Step 2: verify the message is visible before clear
            ws.send({"type": "sync", "alias": cls.ALIAS})
            cls.snap_before = ws.recv_snapshot(cls.ALIAS)
        finally:
            ws.close()

        # Drain all LLM responses for this message so nothing is in-flight
        # when the clear is issued.  The clear must see the COMPLETE conversation
        # (user + any assistant turns) in the ring buffer to remove all of it.
        _drain_ws_frames()
        _board_ready()   # drain can temporarily stress HTTP; re-confirm readiness

        # Step 3: clear via REST — LLM is idle, ring buffer has full conversation
        r = session_post({"action": "clear"})
        cls.clear_status = r.status_code
        time.sleep(0.3)

        # Step 4: fresh connection — sync must return empty messages
        ws2 = _WsClient()
        try:
            ws2.send({"type": "sync", "alias": cls.ALIAS})
            cls.snap_after = ws2.recv_snapshot(cls.ALIAS)
        finally:
            ws2.close()

    @classmethod
    def tearDownClass(cls):
        time.sleep(_WS_TEARDOWN_SETTLE)

    def test_clear_returns_200(self):
        self.assertEqual(self.clear_status, 200)

    def test_msg_present_before_clear(self):
        """Pre-condition: message must be visible in ring buffer before clear."""
        self.assertIsNotNone(self.snap_before, "Sync before clear returned no snapshot")
        texts = [m.get("text", "") for m in self.snap_before.get("messages", [])]
        self.assertIn(self.MSG, texts,
                      f"Pre-condition failed: message not in snapshot before clear. "
                      f"Got: {texts}")

    def test_snapshot_received_after_clear(self):
        """A fresh sync after clear must still receive a snapshot frame."""
        self.assertIsNotNone(self.snap_after,
                             "No snapshot received after clear — sync timed out")

    def test_messages_empty_after_clear(self):
        """
        Core regression: after clear, the sync snapshot must carry zero messages.
        Before the fix, s_msgs[] retained entries for the cleared alias, so
        push_session_history() returned stale messages on the next sync.
        """
        self.assertIsNotNone(self.snap_after)
        msgs = self.snap_after.get("messages", [])
        self.assertEqual(
            len(msgs), 0,
            f"Ring buffer not cleared: {len(msgs)} stale message(s) returned after "
            f"clear. Texts: {[m.get('text', '') for m in msgs]}"
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
