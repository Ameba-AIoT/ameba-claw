"""
VFS fallback and session_snapshot field tests.

WHY these tests exist:

  B-03 ~ B-10  (VFS fallback — claw_memory_read_session_json):
    cap_im_local's ring buffer (s_msgs[]) lives only in RAM.  After a device
    reboot the ring buffer is gone, but the VFS session file survives.  When
    the WebUI syncs an alias whose ring buffer is empty, push_session_history
    falls back to reading the VFS file via claw_memory_read_session_json.
    These tests drive every documented edge-case of that reader:
      completed:true   — normal completed turn
      completed:false  — interrupted turn, must substitute placeholder text
      no completed key — legacy format, must be treated as complete
      mixed turns      — all three kinds in one file
      no file          — graceful empty response, no crash
      empty turns []   — graceful empty response
      corrupt JSON     — graceful empty response, no crash or hang
      empty user text  — turn must not be silently dropped
    Without this coverage, regressions in the fallback path only surface as
    missing conversation history after a reboot.

  C-04  (session_snapshot no history field):
    An earlier version of broadcast_session_snapshot included a "history"
    array in the WS frame.  That field was removed in this PR: history is
    fetched on demand via {type:"sync"}, not pushed on every session switch.
    This test is the regression guard — if "history" reappears, the client
    receives stale unbounded data on every resume/new action.
"""
import json
import sys
import os
import time
import unittest

import requests
import websocket

sys.path.insert(0, os.path.dirname(__file__))
from config import BOARD_BASE_URL, HTTP_TIMEOUT

BASE = BOARD_BASE_URL
SESSION_URL = BASE + "/api/session"
WS_URL = "ws://127.0.0.1/ws/chat"

WS_RECV_TIMEOUT = 5.0
_WS_TEARDOWN_SETTLE = 4.0


# ---------------------------------------------------------------------------
# Filename helpers (mirrors cap_session_mgr.c)
# ---------------------------------------------------------------------------

def _djb2_add(s: str) -> int:
    """Addition-variant djb2 used by cap_session_mgr.c (chat_map files)."""
    h = 5381
    for c in s.encode("utf-8"):
        h = ((h << 5) + h + c) & 0xFFFFFFFF
    return h


def _djb2_xor(s: str) -> int:
    """XOR-variant djb2 used by claw_memory.c (session history files)."""
    h = 5381
    for c in s.encode("utf-8"):
        h = (((h << 5) + h) ^ c) & 0xFFFFFFFF
    return h


def _sanitize(s: str, max_len: int = 32) -> str:
    return "".join(c if (c.isalnum() or c in "._-") else "_" for c in s)[:max_len]


def session_history_path(channel: str, chat_id: str, alias: str) -> str:
    """XOR hash — matches claw_memory.c session_file_path()."""
    sid = f"{channel}:{chat_id}:{alias}"
    return f"/session/s_{_sanitize(sid)}_{_djb2_xor(sid):08x}.json"


def chat_map_path(channel: str, chat_id: str) -> str:
    """Addition hash — matches cap_session_mgr.c hash_str()."""
    key = f"{channel}:{chat_id}"
    return f"/session/chat_map/s_{_sanitize(key)}_{_djb2_add(key):08x}.json"


# ---------------------------------------------------------------------------
# REST helpers
# ---------------------------------------------------------------------------

def files_delete(path: str):
    requests.delete(BASE + "/api/files", params={"path": path}, timeout=HTTP_TIMEOUT)


def files_put(path: str, content: str):
    requests.put(
        BASE + "/api/files/content",
        params={"path": path},
        data=content.encode("utf-8"),
        timeout=HTTP_TIMEOUT,
    )


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

    def recv_session_snapshot(self, timeout: float = WS_RECV_TIMEOUT):
        """Wait for a {type:'session_snapshot'} broadcast frame."""
        return self.recv_until(
            lambda d: d.get("type") == "session_snapshot",
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


# ---------------------------------------------------------------------------
# Board readiness helper (mirrors test_session_ws_isolation.py)
# ---------------------------------------------------------------------------

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
# B-03: VFS fallback — completed:true turn
# ---------------------------------------------------------------------------

@_skip
class TestVfsFallbackBasic(unittest.TestCase):
    """
    Completed turn is read from VFS when RAM ring buffer is empty.

    WHY: The most common fallback scenario — device rebooted, history on disk,
    UI requests sync.  Verifies that a completed:true turn is returned as both
    a user message and an assistant message.
    """

    ALIAS = "vfb_basic"
    USER_TEXT = "hello from vfs"
    ASST_TEXT = "vfs reply here"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        # Create alias in RAM with empty ring buffer (no messages sent)
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.1)
        # Write VFS session file directly; firmware falls back to it on sync
        vfs_content = json.dumps({
            "turns": [
                {
                    "user": cls.USER_TEXT,
                    "assistant": cls.ASST_TEXT,
                    "completed": True,
                    "req_id": 1001,
                }
            ]
        })
        files_put(session_history_path("local", "local", cls.ALIAS), vfs_content)
        time.sleep(0.1)
        ws = _WsClient()
        try:
            ws.send({"type": "sync", "alias": cls.ALIAS})
            cls.snap = ws.recv_snapshot(cls.ALIAS)
        finally:
            ws.close()

    @classmethod
    def tearDownClass(cls):
        try:
            files_delete(session_history_path("local", "local", cls.ALIAS))
        except Exception:
            pass
        try:
            session_delete(cls.ALIAS)
        except Exception:
            pass
        time.sleep(_WS_TEARDOWN_SETTLE)

    def test_snapshot_received(self):
        self.assertIsNotNone(self.snap, "No snapshot received from WS sync")

    def test_messages_contain_user(self):
        self.assertIsNotNone(self.snap)
        msgs = self.snap.get("messages", [])
        user_texts = [m["text"] for m in msgs if m.get("role") == "user"]
        self.assertIn(self.USER_TEXT, user_texts,
                      f"User text not in snapshot messages: {msgs}")

    def test_messages_contain_assistant(self):
        self.assertIsNotNone(self.snap)
        msgs = self.snap.get("messages", [])
        asst_texts = [m["text"] for m in msgs if m.get("role") == "assistant"]
        self.assertIn(self.ASST_TEXT, asst_texts,
                      f"Assistant text not in snapshot messages: {msgs}")


# ---------------------------------------------------------------------------
# B-04: VFS fallback — completed:false turn
# ---------------------------------------------------------------------------

@_skip
class TestVfsFallbackCompletedFalse(unittest.TestCase):
    """
    Interrupted turn (completed:false, no assistant) returns user text and
    the "ameba claw is interrupted!" placeholder.

    WHY: A request interrupted mid-flight (power loss, reset mid-inference)
    leaves completed:false in VFS.  The firmware must synthesise the placeholder
    so the UI shows a clear interruption indicator, not a blank assistant bubble.
    An empty-text message in this context is a bug: it would produce an invisible
    bubble that confuses the UI's message-pair layout.
    """

    ALIAS = "vfb_incomplete"
    USER_TEXT = "a question that was interrupted"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.1)
        vfs_content = json.dumps({
            "turns": [
                {
                    "user": cls.USER_TEXT,
                    "completed": False,
                    "req_id": 1002,
                }
            ]
        })
        files_put(session_history_path("local", "local", cls.ALIAS), vfs_content)
        time.sleep(0.1)
        ws = _WsClient()
        try:
            ws.send({"type": "sync", "alias": cls.ALIAS})
            cls.snap = ws.recv_snapshot(cls.ALIAS)
        finally:
            ws.close()

    @classmethod
    def tearDownClass(cls):
        try:
            files_delete(session_history_path("local", "local", cls.ALIAS))
        except Exception:
            pass
        try:
            session_delete(cls.ALIAS)
        except Exception:
            pass
        time.sleep(_WS_TEARDOWN_SETTLE)

    def test_snapshot_received(self):
        self.assertIsNotNone(self.snap, "No snapshot received")

    def test_user_text_present(self):
        self.assertIsNotNone(self.snap)
        msgs = self.snap.get("messages", [])
        user_texts = [m["text"] for m in msgs if m.get("role") == "user"]
        self.assertIn(self.USER_TEXT, user_texts,
                      f"User text missing from incomplete turn: {msgs}")

    def test_interrupted_placeholder_present(self):
        self.assertIsNotNone(self.snap)
        msgs = self.snap.get("messages", [])
        asst_texts = [m["text"] for m in msgs if m.get("role") == "assistant"]
        self.assertTrue(
            any("interrupted" in t for t in asst_texts),
            f"Expected 'ameba claw is interrupted!' placeholder, got: {asst_texts}",
        )

    def test_no_empty_text(self):
        """No message should carry an empty text string."""
        self.assertIsNotNone(self.snap)
        msgs = self.snap.get("messages", [])
        empty = [m for m in msgs if m.get("text", None) == ""]
        self.assertEqual(
            len(empty), 0,
            f"Messages with empty text string found: {empty}",
        )


# ---------------------------------------------------------------------------
# B-05: VFS fallback — legacy turn (no "completed" field)
# ---------------------------------------------------------------------------

@_skip
class TestVfsFallbackLegacyTurn(unittest.TestCase):
    """
    Turn without the "completed" key (legacy format) is treated as complete.

    WHY: Old firmware versions wrote turns without the "completed" key.  The
    reader must not emit the interrupted placeholder for such turns — the
    absence of the field is not the same as completed:false.
    """

    ALIAS = "vfb_legacy"
    USER_TEXT = "legacy user message"
    ASST_TEXT = "legacy assistant reply"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.1)
        # No "completed" field — legacy format
        vfs_content = json.dumps({
            "turns": [
                {
                    "user": cls.USER_TEXT,
                    "assistant": cls.ASST_TEXT,
                }
            ]
        })
        files_put(session_history_path("local", "local", cls.ALIAS), vfs_content)
        time.sleep(0.1)
        ws = _WsClient()
        try:
            ws.send({"type": "sync", "alias": cls.ALIAS})
            cls.snap = ws.recv_snapshot(cls.ALIAS)
        finally:
            ws.close()

    @classmethod
    def tearDownClass(cls):
        try:
            files_delete(session_history_path("local", "local", cls.ALIAS))
        except Exception:
            pass
        try:
            session_delete(cls.ALIAS)
        except Exception:
            pass
        time.sleep(_WS_TEARDOWN_SETTLE)

    def test_snapshot_received(self):
        self.assertIsNotNone(self.snap, "No snapshot received")

    def test_user_text_present(self):
        self.assertIsNotNone(self.snap)
        msgs = self.snap.get("messages", [])
        user_texts = [m["text"] for m in msgs if m.get("role") == "user"]
        self.assertIn(self.USER_TEXT, user_texts,
                      f"User text missing: {msgs}")

    def test_assistant_text_present(self):
        self.assertIsNotNone(self.snap)
        msgs = self.snap.get("messages", [])
        asst_texts = [m["text"] for m in msgs if m.get("role") == "assistant"]
        self.assertIn(self.ASST_TEXT, asst_texts,
                      f"Assistant text missing: {msgs}")

    def test_no_interrupted_placeholder(self):
        """Legacy turn (no completed field) must not produce the interrupted placeholder."""
        self.assertIsNotNone(self.snap)
        msgs = self.snap.get("messages", [])
        asst_texts = [m["text"] for m in msgs if m.get("role") == "assistant"]
        self.assertFalse(
            any("interrupted" in t for t in asst_texts),
            f"Unexpected 'interrupted' placeholder in legacy turn: {asst_texts}",
        )


# ---------------------------------------------------------------------------
# B-06: VFS fallback — mixed turns
# ---------------------------------------------------------------------------

@_skip
class TestVfsFallbackMixedTurns(unittest.TestCase):
    """
    Three turns (completed, legacy, incomplete) → 6 messages, exactly 1 interrupted.

    WHY: Real VFS files from long sessions contain a mix of turn types.
    All turns must be surfaced and the interrupted placeholder must appear
    exactly once — once per incomplete turn, not more, not fewer.
    """

    ALIAS = "vfb_mixed"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.1)
        vfs_content = json.dumps({
            "turns": [
                # Turn 1: completed
                {"user": "q1", "assistant": "a1", "completed": True, "req_id": 2001},
                # Turn 2: legacy (no completed field)
                {"user": "q2", "assistant": "a2"},
                # Turn 3: incomplete
                {"user": "q3", "completed": False, "req_id": 2003},
            ]
        })
        files_put(session_history_path("local", "local", cls.ALIAS), vfs_content)
        time.sleep(0.1)
        ws = _WsClient()
        try:
            ws.send({"type": "sync", "alias": cls.ALIAS})
            cls.snap = ws.recv_snapshot(cls.ALIAS)
        finally:
            ws.close()

    @classmethod
    def tearDownClass(cls):
        try:
            files_delete(session_history_path("local", "local", cls.ALIAS))
        except Exception:
            pass
        try:
            session_delete(cls.ALIAS)
        except Exception:
            pass
        time.sleep(_WS_TEARDOWN_SETTLE)

    def test_snapshot_received(self):
        self.assertIsNotNone(self.snap, "No snapshot received")

    def test_six_messages(self):
        """3 turns × 2 roles = 6 messages total."""
        self.assertIsNotNone(self.snap)
        msgs = self.snap.get("messages", [])
        self.assertEqual(len(msgs), 6,
                         f"Expected 6 messages, got {len(msgs)}: {msgs}")

    def test_exactly_one_interrupted(self):
        self.assertIsNotNone(self.snap)
        msgs = self.snap.get("messages", [])
        interrupted = [m for m in msgs
                       if m.get("role") == "assistant"
                       and "interrupted" in m.get("text", "")]
        self.assertEqual(
            len(interrupted), 1,
            f"Expected exactly 1 interrupted placeholder, got {len(interrupted)}: {msgs}",
        )


# ---------------------------------------------------------------------------
# B-07: VFS fallback — no VFS file
# ---------------------------------------------------------------------------

@_skip
class TestVfsFallbackNoFile(unittest.TestCase):
    """
    No VFS file → snapshot received with empty messages list.

    WHY: A freshly created alias (or a session whose VFS file was deleted) must
    not cause a crash or hang.  The fallback must gracefully return empty
    messages so the UI renders a blank conversation, not an error.
    """

    ALIAS = "vfb_nofile"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        # Ensure no VFS file exists for this alias
        files_delete(session_history_path("local", "local", cls.ALIAS))
        # Create alias in RAM (empty ring buffer, no VFS file)
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
        try:
            session_delete(cls.ALIAS)
        except Exception:
            pass
        time.sleep(_WS_TEARDOWN_SETTLE)

    def test_snapshot_received(self):
        self.assertIsNotNone(self.snap,
                             "Firmware timed out or crashed — no snapshot received")

    def test_messages_empty(self):
        self.assertIsNotNone(self.snap)
        msgs = self.snap.get("messages", [])
        self.assertEqual(len(msgs), 0,
                         f"Expected empty messages for non-existent VFS file, got: {msgs}")


# ---------------------------------------------------------------------------
# B-08: VFS fallback — empty turns array
# ---------------------------------------------------------------------------

@_skip
class TestVfsFallbackEmptyTurnsArray(unittest.TestCase):
    """
    VFS file with {"turns":[]} → snapshot received with empty messages.

    WHY: A session that was created but never had any LLM turns writes an
    empty turns array.  The reader must handle this without returning stale
    data from a previous read or crashing.
    """

    ALIAS = "vfb_empty_arr"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.1)
        files_put(session_history_path("local", "local", cls.ALIAS),
                  json.dumps({"turns": []}))
        time.sleep(0.1)
        ws = _WsClient()
        try:
            ws.send({"type": "sync", "alias": cls.ALIAS})
            cls.snap = ws.recv_snapshot(cls.ALIAS)
        finally:
            ws.close()

    @classmethod
    def tearDownClass(cls):
        try:
            files_delete(session_history_path("local", "local", cls.ALIAS))
        except Exception:
            pass
        try:
            session_delete(cls.ALIAS)
        except Exception:
            pass
        time.sleep(_WS_TEARDOWN_SETTLE)

    def test_snapshot_received(self):
        self.assertIsNotNone(self.snap, "No snapshot received")

    def test_messages_empty(self):
        self.assertIsNotNone(self.snap)
        msgs = self.snap.get("messages", [])
        self.assertEqual(len(msgs), 0,
                         f"Expected empty messages for empty turns array, got: {msgs}")


# ---------------------------------------------------------------------------
# B-09: VFS fallback — corrupt file
# ---------------------------------------------------------------------------

@_skip
class TestVfsFallbackCorruptFile(unittest.TestCase):
    """
    Corrupt VFS file → firmware does not crash; snapshot received with empty messages.

    WHY: Power loss mid-write can corrupt a VFS file.  The fallback reader must
    handle JSON parse failures gracefully and not block, hang, or crash —
    the WS sync must still return a valid (empty) snapshot frame.
    """

    ALIAS = "vfb_corrupt"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.1)
        # Deliberately invalid JSON
        files_put(session_history_path("local", "local", cls.ALIAS), "{invalid]}")
        time.sleep(0.1)
        ws = _WsClient()
        try:
            ws.send({"type": "sync", "alias": cls.ALIAS})
            # Use a longer timeout — corrupt-file error handling may be slower
            cls.snap = ws.recv_snapshot(cls.ALIAS, timeout=8.0)
        finally:
            ws.close()

    @classmethod
    def tearDownClass(cls):
        try:
            files_delete(session_history_path("local", "local", cls.ALIAS))
        except Exception:
            pass
        try:
            session_delete(cls.ALIAS)
        except Exception:
            pass
        time.sleep(_WS_TEARDOWN_SETTLE)

    def test_firmware_did_not_crash(self):
        """Firmware must respond (not crash or hang) when the VFS file is corrupt."""
        self.assertIsNotNone(
            self.snap,
            "No snapshot received — firmware may have crashed or hung on corrupt JSON",
        )

    def test_messages_empty_on_corrupt(self):
        """Corrupt file must not produce partial or garbage messages."""
        if self.snap is None:
            self.skipTest("No snapshot received (see test_firmware_did_not_crash)")
        msgs = self.snap.get("messages", [])
        self.assertEqual(len(msgs), 0,
                         f"Expected empty messages on corrupt file, got: {msgs}")


# ---------------------------------------------------------------------------
# B-10: VFS fallback — empty user text
# ---------------------------------------------------------------------------

@_skip
class TestVfsFallbackEmptyUserText(unittest.TestCase):
    """
    Turn with user=="" must not be silently dropped by the fallback reader.

    WHY: A turn where user is the empty string is valid (e.g. a voice input with
    empty transcription).  Silently skipping it misaligns message-pair numbering:
    the UI maps assistant replies to user bubbles by position, so dropping one
    user message shifts every subsequent reply to the wrong bubble.
    The firmware must emit role:user text="" to preserve the pairing.
    """

    ALIAS = "vfb_empty_usr"
    ASST_TEXT = "reply to empty input"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.1)
        vfs_content = json.dumps({
            "turns": [
                {
                    "user": "",
                    "assistant": cls.ASST_TEXT,
                    "completed": True,
                    "req_id": 3001,
                }
            ]
        })
        files_put(session_history_path("local", "local", cls.ALIAS), vfs_content)
        time.sleep(0.1)
        ws = _WsClient()
        try:
            ws.send({"type": "sync", "alias": cls.ALIAS})
            cls.snap = ws.recv_snapshot(cls.ALIAS)
        finally:
            ws.close()

    @classmethod
    def tearDownClass(cls):
        try:
            files_delete(session_history_path("local", "local", cls.ALIAS))
        except Exception:
            pass
        try:
            session_delete(cls.ALIAS)
        except Exception:
            pass
        time.sleep(_WS_TEARDOWN_SETTLE)

    def test_snapshot_received(self):
        self.assertIsNotNone(self.snap, "No snapshot received")

    def test_user_message_with_empty_text_present(self):
        """role:user text="" must be present — firmware must not skip the turn."""
        self.assertIsNotNone(self.snap)
        msgs = self.snap.get("messages", [])
        user_msgs = [m for m in msgs if m.get("role") == "user"]
        self.assertTrue(
            any(m.get("text") == "" for m in user_msgs),
            f"Expected a user message with text='', got user messages: {user_msgs}",
        )

    def test_assistant_reply_present(self):
        self.assertIsNotNone(self.snap)
        msgs = self.snap.get("messages", [])
        asst_texts = [m["text"] for m in msgs if m.get("role") == "assistant"]
        self.assertIn(self.ASST_TEXT, asst_texts,
                      f"Assistant reply missing from snapshot: {msgs}")


# ---------------------------------------------------------------------------
# C-04: session_snapshot broadcast must not contain a "history" field
# ---------------------------------------------------------------------------

@_skip
class TestSessionSnapshotNoHistory(unittest.TestCase):
    """
    broadcast_session_snapshot must not include a "history" field in its WS frame.

    WHY: An earlier version included a redundant "history" array in the
    {type:"session_snapshot"} frame broadcast on every action=new / action=resume.
    The field was removed in this PR: history is now fetched on demand via
    {type:"sync"}, keeping session-switch frames lightweight.  If "history"
    reappears, the client silently receives stale unbounded data on every
    session switch — a bandwidth and correctness regression.
    """

    ALIAS = "snap_no_hist"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.1)

        ws = _WsClient()
        try:
            # action=resume triggers broadcast_session_snapshot to all WS clients
            session_post({"action": "resume", "alias": cls.ALIAS})
            cls.snap = ws.recv_session_snapshot(timeout=5.0)
        finally:
            ws.close()

    @classmethod
    def tearDownClass(cls):
        try:
            session_delete(cls.ALIAS)
        except Exception:
            pass
        time.sleep(_WS_TEARDOWN_SETTLE)

    def test_session_snapshot_received(self):
        self.assertIsNotNone(
            self.snap,
            "No session_snapshot frame received after action=resume — "
            "either the broadcast was not sent or the frame type differs from 'session_snapshot'",
        )

    def test_no_history_field(self):
        """session_snapshot frame must not carry a 'history' field."""
        self.assertIsNotNone(self.snap)
        self.assertNotIn(
            "history", self.snap,
            f"'history' field present in session_snapshot frame: {list(self.snap.keys())}",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
