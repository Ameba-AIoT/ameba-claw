"""
Slash command routing and alias-explicit operation tests.

WHY these tests exist (gap not covered by test_session_ws_isolation.py):

  1. send_reply() used cap_im_local_send() -> get_current(), so slash command
     replies went to whatever 'current' was at reply time. /new changes current
     before the reply is sent, so the reply was landing in the NEW session
     instead of the originating one.

  2. cap_session_mgr_rename() / cap_session_mgr_clear_chat() always targeted
     'current'. Typing /rename or /clear in session B while current=A would
     silently modify A instead of B.

  3. Lazy current: current should update only when the user sends a real message
     (LLM request start), NOT on WS sync ({type:'sync',alias:X}).

WHAT is tested here:
  T1. /new reply alias == originating alias (not the newly created session name).
  T2. /rename sent from alias=B (current=A): B is renamed, A is unchanged,
      current stays A.
  T3. /clear sent from alias=B (current=A): reply alias == B (routing correct).
  T4. WS {type:'sync'} alone does NOT update current.
  T5. User message ({text,alias:B}) updates current to B.
"""
import json
import os
import sys
import time
import unittest

import requests
import websocket

sys.path.insert(0, os.path.dirname(__file__))
from config import BOARD_BASE_URL, HTTP_TIMEOUT

BASE        = BOARD_BASE_URL
SESSION_URL = BASE + "/api/session"
FILES_URL   = BASE + "/api/files"
WS_URL      = "ws://127.0.0.1/ws/chat"

WS_RECV_TIMEOUT  = 6.0   # seconds to wait for a slash-command reply frame
LLM_QUIET_SECS   = 8.0   # silence window that means LLM is done
LLM_DRAIN_MAX    = 40.0


# ---------------------------------------------------------------------------
# Filename helpers (mirrors cap_session_mgr.c)
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

def session_post(body: dict) -> requests.Response:
    return requests.post(SESSION_URL, json=body, timeout=HTTP_TIMEOUT)

def session_delete(alias: str) -> requests.Response:
    return requests.delete(SESSION_URL, params={"alias": alias}, timeout=HTTP_TIMEOUT)

def session_get() -> dict:
    return requests.get(SESSION_URL, timeout=HTTP_TIMEOUT).json()

def files_delete(path: str):
    try:
        requests.delete(FILES_URL, params={"path": path}, timeout=HTTP_TIMEOUT)
    except Exception:
        pass

def reset_chat_map():
    """Delete chat_map so next operation starts from a clean state."""
    files_delete(chat_map_path("local", "local"))
    time.sleep(0.3)


# ---------------------------------------------------------------------------
# WebSocket helpers
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

    def recv_slash_reply(self, alias: str, timeout: float = WS_RECV_TIMEOUT):
        """Wait for an assistant frame with a specific alias (slash cmd reply)."""
        return self.recv_until(
            lambda d: d.get("role") == "assistant" and d.get("alias") == alias,
            timeout=timeout,
        )

    def close(self):
        try:
            self._ws.close()
        except Exception:
            pass


def _drain_ws_frames(quiet_secs: float = LLM_QUIET_SECS,
                     max_secs: float = LLM_DRAIN_MAX) -> None:
    """Discard WS frames until quiet — waits for any in-flight LLM response."""
    try:
        ws = websocket.WebSocket()
        ws.settimeout(quiet_secs)
        ws.connect(WS_URL)
        deadline = time.time() + max_secs
        while time.time() < deadline:
            try:
                ws.recv()
            except websocket.WebSocketTimeoutException:
                break
            except Exception:
                break
        try:
            ws.close()
        except Exception:
            pass
    except Exception:
        pass


def _ws_available() -> bool:
    try:
        c = _WsClient()
        c.close()
        return True
    except Exception:
        return False


_no_ws = not _ws_available()
_skip  = unittest.skipIf(_no_ws, "WebSocket /ws/chat not reachable")

# 4-second settle between classes so the board frees WS slots from teardown.
_WS_TEARDOWN_SETTLE = 4.0


# ---------------------------------------------------------------------------
# T1 — /new reply goes to the originating session, not the new session
# ---------------------------------------------------------------------------

@_skip
class TestSlashNewReplyRoutedToOriginatingAlias(unittest.TestCase):
    """
    /new <name> is sent with alias=ORIG.
    Before the fix: after cap_session_mgr_new() the server current=<name>,
    so cap_im_local_send() broadcast the reply with alias=<name>, which was
    filtered out by a browser still viewing ORIG.
    After the fix: send_reply uses ev->message_id (ORIG) as chat_id, so
    cap_im_local_send(ORIG, ...) broadcasts with alias=ORIG.
    """
    ORIG   = "slash_new_orig"
    NEWSES = "slash_new_created"

    @classmethod
    def setUpClass(cls):
        reset_chat_map()
        # Create and make current the originating session
        session_post({"action": "new", "alias": cls.ORIG})

    @classmethod
    def tearDownClass(cls):
        _drain_ws_frames()
        time.sleep(_WS_TEARDOWN_SETTLE)
        for a in (cls.ORIG, cls.NEWSES):
            try:
                session_delete(a)
            except Exception:
                pass

    def test_new_reply_alias_equals_originating_session(self):
        ws = _WsClient()
        try:
            # Sync to ORIG to establish context (mirrors browser behaviour)
            ws.send({"type": "sync", "alias": self.ORIG})
            ws.recv_until(lambda d: d.get("type") == "snapshot", timeout=4.0)

            # Send /new from alias=ORIG
            ws.send({"text": f"/new {self.NEWSES}", "alias": self.ORIG})

            # Expect reply with alias=ORIG ("✓ New session … created.")
            reply = ws.recv_slash_reply(self.ORIG, timeout=WS_RECV_TIMEOUT)

            self.assertIsNotNone(
                reply,
                f"No assistant frame with alias='{self.ORIG}' received within "
                f"{WS_RECV_TIMEOUT}s. "
                "Reply may have been routed to the newly-created session instead."
            )
            self.assertEqual(reply.get("alias"), self.ORIG)
            self.assertIn("created", reply.get("text", "").lower())
        finally:
            ws.close()


# ---------------------------------------------------------------------------
# T2 — /rename sent from alias=B (current=A) renames B, leaves A & current
# ---------------------------------------------------------------------------

@_skip
class TestSlashRenameTargetsEventAlias(unittest.TestCase):
    """
    With current=A, send /rename <new> with alias=B.
    Expected: B renamed, A unchanged, current still A.
    Before the fix: cap_session_mgr_rename() always renamed current (A).
    After the fix: rename_alias(B, new) is called; rename_alias only updates
    current when current==B (here current==A, so current stays A).
    """
    ALIAS_A  = "slash_ren_cur_a"
    ALIAS_B  = "slash_ren_src_b"
    ALIAS_NEW = "slash_ren_renamed"

    @classmethod
    def setUpClass(cls):
        reset_chat_map()
        # Create A (becomes current) then B
        session_post({"action": "new", "alias": cls.ALIAS_A})
        session_post({"action": "new", "alias": cls.ALIAS_B})
        # Restore current to A
        session_post({"action": "resume", "alias": cls.ALIAS_A})
        cls.current_before = session_get().get("current")

    @classmethod
    def tearDownClass(cls):
        time.sleep(_WS_TEARDOWN_SETTLE)
        for a in (cls.ALIAS_A, cls.ALIAS_B, cls.ALIAS_NEW):
            try:
                session_delete(a)
            except Exception:
                pass

    def test_precondition_current_is_a(self):
        self.assertEqual(self.current_before, self.ALIAS_A,
                         "Pre-condition failed: current must be A before test")

    def test_rename_targets_b_not_a(self):
        ws = _WsClient()
        try:
            ws.send({"type": "sync", "alias": self.ALIAS_B})
            ws.recv_until(lambda d: d.get("type") == "snapshot", timeout=4.0)

            ws.send({"text": f"/rename {self.ALIAS_NEW}", "alias": self.ALIAS_B})
            reply = ws.recv_slash_reply(self.ALIAS_B, timeout=WS_RECV_TIMEOUT)
        finally:
            ws.close()

        self.assertIsNotNone(reply,
            f"No reply received for alias='{self.ALIAS_B}'. "
            "Reply may have been routed to current session A instead.")
        self.assertIn("renamed", reply.get("text", "").lower())

        state = session_get()
        sessions = state.get("sessions", [])
        aliases = [s["alias"] if isinstance(s, dict) else s for s in sessions]

        self.assertIn(self.ALIAS_NEW, aliases,
                      "New alias not found — rename may have targeted wrong session")
        self.assertNotIn(self.ALIAS_B, aliases,
                         "Old alias B still present — rename did not remove it")
        self.assertIn(self.ALIAS_A, aliases,
                      "Session A disappeared — rename may have targeted A instead of B")

    def test_current_unchanged_after_rename(self):
        state = session_get()
        self.assertEqual(state.get("current"), self.ALIAS_A,
                         "current changed after /rename; should stay A")


# ---------------------------------------------------------------------------
# T3 — /clear sent from alias=B (current=A) reply routes to B
# ---------------------------------------------------------------------------

@_skip
class TestSlashClearReplyRoutedToEventAlias(unittest.TestCase):
    """
    With current=A, send /clear with alias=B.
    The reply must arrive with alias=B (not A).
    This verifies both the send_reply routing fix and clear_chat_alias.
    """
    ALIAS_A = "slash_clr_cur_a"
    ALIAS_B = "slash_clr_src_b"

    @classmethod
    def setUpClass(cls):
        reset_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS_A})
        session_post({"action": "new", "alias": cls.ALIAS_B})
        session_post({"action": "resume", "alias": cls.ALIAS_A})
        cls.current_before = session_get().get("current")

    @classmethod
    def tearDownClass(cls):
        time.sleep(_WS_TEARDOWN_SETTLE)
        for a in (cls.ALIAS_A, cls.ALIAS_B):
            try:
                session_delete(a)
            except Exception:
                pass

    def test_precondition_current_is_a(self):
        self.assertEqual(self.current_before, self.ALIAS_A,
                         "Pre-condition failed: current must be A before test")

    def test_clear_reply_alias_equals_b(self):
        ws = _WsClient()
        try:
            ws.send({"type": "sync", "alias": self.ALIAS_B})
            ws.recv_until(lambda d: d.get("type") == "snapshot", timeout=4.0)

            ws.send({"text": "/clear", "alias": self.ALIAS_B})
            reply = ws.recv_slash_reply(self.ALIAS_B, timeout=WS_RECV_TIMEOUT)
        finally:
            ws.close()

        self.assertIsNotNone(reply,
            f"No reply received for alias='{self.ALIAS_B}'. "
            "Reply may have been routed to current session A instead.")
        self.assertEqual(reply.get("alias"), self.ALIAS_B)
        self.assertIn("cleared", reply.get("text", "").lower())

    def test_current_unchanged_after_clear(self):
        state = session_get()
        self.assertEqual(state.get("current"), self.ALIAS_A,
                         "current changed after /clear; should stay A")


# ---------------------------------------------------------------------------
# T4/T5 — Lazy current update
# ---------------------------------------------------------------------------

@_skip
class TestLazyCurrentUpdate(unittest.TestCase):
    """
    T4: WS {type:'sync', alias:B} alone must NOT update current.
    T5: WS {text:..., alias:B} (user message) MUST update current to B.
    """
    ALIAS_A = "lazy_cur_a"
    ALIAS_B = "lazy_cur_b"

    @classmethod
    def setUpClass(cls):
        reset_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS_A})
        session_post({"action": "new", "alias": cls.ALIAS_B})
        # current = A
        session_post({"action": "resume", "alias": cls.ALIAS_A})
        cls.current_before = session_get().get("current")

    @classmethod
    def tearDownClass(cls):
        _drain_ws_frames()   # drain LLM triggered by user message in T5
        time.sleep(_WS_TEARDOWN_SETTLE)
        for a in (cls.ALIAS_A, cls.ALIAS_B):
            try:
                session_delete(a)
            except Exception:
                pass

    def test_precondition_current_is_a(self):
        self.assertEqual(self.current_before, self.ALIAS_A,
                         "Pre-condition failed")

    def test_sync_alone_does_not_update_current(self):
        """T4: {type:'sync', alias:B} must not call resume — current stays A."""
        # Reset to A so this test is order-independent (pytest-randomly may run T5 first)
        session_post({"action": "resume", "alias": self.ALIAS_A})
        time.sleep(0.3)
        ws = _WsClient()
        try:
            ws.send({"type": "sync", "alias": self.ALIAS_B})
            # Wait for snapshot (confirms the sync was processed)
            ws.recv_until(lambda d: d.get("type") == "snapshot", timeout=4.0)
        finally:
            ws.close()

        time.sleep(0.5)  # brief settle for any async file I/O
        state = session_get()
        self.assertEqual(state.get("current"), self.ALIAS_A,
                         "current changed after WS sync — lazy update triggered too early")

    def test_user_message_updates_current(self):
        """T5: {text:..., alias:B} triggers cap_session_mgr_resume(B) → current=B."""
        # Reset to A so this test is order-independent (pytest-randomly may run T4 first)
        session_post({"action": "resume", "alias": self.ALIAS_A})
        time.sleep(0.3)
        ws = _WsClient()
        try:
            ws.send({"type": "sync", "alias": self.ALIAS_B})
            ws.recv_until(lambda d: d.get("type") == "snapshot", timeout=4.0)
            # Send a real user message — this is the LLM request start
            ws.send({"text": "hi", "alias": self.ALIAS_B})
            time.sleep(1.0)  # let ws_on_message call cap_session_mgr_resume
        finally:
            ws.close()

        state = session_get()
        self.assertEqual(state.get("current"), self.ALIAS_B,
                         "current not updated after user message — lazy current update broken")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    unittest.main(verbosity=2)
