"""
WHY this test exists (C-01 — NO_ACK flag regression):

  When a user sends a message, cap_im_local was firing an early "working on
  it…" ACK frame via cap_im_local_send().  That call routed through
  cap_session_mgr_get_current(), so the ACK was ALWAYS delivered to the
  server's CURRENT session — regardless of which alias actually sent the
  message.

  Broken scenario:
    server current  = alias A
    WS message sent with alias = B   (different from current)
    → old: ACK delivered to A        (wrong alias — this is the bug)
    → new: CLAW_IM_CHANNEL_FLAG_NO_ACK on local channel suppresses ACK entirely

WHAT is tested:
  1. Alias A (the current session) receives 0 assistant frames in 4 s.
     Before the fix the ACK leaked here via get_current().
  2. Alias B (the originating session) also receives 0 assistant frames in 4 s.
     NO_ACK suppresses the early ACK — not just reroutes it.
  3. No untagged (alias empty / None) assistant frames arrive in the window.

WHY 4 seconds:
  LLM inference takes 20+ seconds.  Any early/synchronous ACK will appear
  within milliseconds — well inside the 4 s observation window — without
  needing to wait for the LLM.
"""
import json
import os
import sys
import threading
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
ACK_WINDOW = 4.0        # observation window (s); LLM takes 20+ s → any ACK appears here

_WS_TEARDOWN_SETTLE = 4.0   # wait for board to free WS slots after close


# ---------------------------------------------------------------------------
# Filename helpers — mirrors cap_session_mgr.c (same as test_session_ws_isolation.py)
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

def session_post(body: dict):
    return requests.post(SESSION_URL, json=body, timeout=HTTP_TIMEOUT)


def _board_ready(timeout: float = 10.0) -> bool:
    """Wait until POST /api/session responds in under 2 s (board not busy)."""
    deadline = time.time() + timeout
    probe = "_probe_noack_ready_"
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
_skip_ws = unittest.skipIf(_no_ws, "WebSocket /ws/chat not reachable")


# ---------------------------------------------------------------------------
# C-01: NO_ACK flag — no early ACK frame on any session within ACK_WINDOW
# ---------------------------------------------------------------------------

@_skip_ws
class TestNoAckFlag(unittest.TestCase):
    """
    C-01: CLAW_IM_CHANNEL_FLAG_NO_ACK must prevent any early assistant frame
    from reaching either session within the 4 s observation window.

    Setup:
      server current = ALIAS_A
      WS message sent with alias = ALIAS_B  (different from current)

    Both WS connections are monitored simultaneously for ACK_WINDOW seconds.
    No assistant frame should appear on either alias — the NO_ACK flag
    suppresses the early ACK entirely.
    """

    ALIAS_A = "noack_cur_a"
    ALIAS_B = "noack_orig_b"
    PROBE_MSG = "noack_probe_7591"

    @classmethod
    def setUpClass(cls):
        _board_ready()

        # Remove local chat_map so stale history cannot confuse alias checks
        try:
            requests.delete(BASE + "/api/files",
                            params={"path": chat_map_path("local", "local")},
                            timeout=HTTP_TIMEOUT)
        except Exception:
            pass
        time.sleep(0.3)

        # Create both aliases fresh
        session_post({"action": "new", "alias": cls.ALIAS_A})
        time.sleep(0.1)
        session_post({"action": "new", "alias": cls.ALIAS_B})
        time.sleep(0.1)

        # Set server current = A
        session_post({"action": "resume", "alias": cls.ALIAS_A})
        time.sleep(0.2)

        # Shared frame buckets populated by two collection threads
        cls.frames_by_alias: dict = {}   # alias -> [frame, ...]
        cls.untagged_frames: list = []   # assistant frames with no/empty alias
        lock = threading.Lock()

        def collect_frames(ws_client: _WsClient) -> None:
            """
            Poll for assistant frames until ACK_WINDOW expires.

            Empty-alias frames are segregated into untagged_frames (宽松处理):
            they may be in-flight LLM responses left over from other test
            classes, so they must not be attributed to ALIAS_A or ALIAS_B.
            The test_no_untagged_assistant_frames test then checks whether any
            such frames arrived at all.
            """
            ws_client._ws.settimeout(0.3)   # short poll so deadline is respected
            deadline = time.time() + ACK_WINDOW
            while time.time() < deadline:
                try:
                    raw = ws_client._ws.recv()
                    d = json.loads(raw)
                    if d.get("role") == "assistant":
                        alias = d.get("alias") or ""
                        with lock:
                            if alias:
                                cls.frames_by_alias.setdefault(alias, []).append(d)
                            else:
                                cls.untagged_frames.append(d)
                except websocket.WebSocketTimeoutException:
                    pass    # keep polling until deadline
                except Exception:
                    break

        # Two connections: observer watches from alias A's context, sender owns B
        observer = _WsClient()
        sender = _WsClient()
        try:
            # Send user message for alias B while server current = A
            sender.send({"text": cls.PROBE_MSG, "alias": cls.ALIAS_B})

            # Monitor both connections simultaneously for ACK_WINDOW seconds
            t_obs = threading.Thread(target=collect_frames, args=(observer,), daemon=True)
            t_snd = threading.Thread(target=collect_frames, args=(sender,), daemon=True)
            t_obs.start()
            t_snd.start()
            t_obs.join(ACK_WINDOW + 1.0)
            t_snd.join(ACK_WINDOW + 1.0)
        finally:
            observer.close()
            sender.close()

    # -- individual assertions -----------------------------------------------

    def test_no_ack_leaked_to_current_session(self):
        """
        ALIAS_A (the server's current session) must receive 0 assistant frames
        within ACK_WINDOW.

        Before CLAW_IM_CHANNEL_FLAG_NO_ACK was added, the early ACK was routed
        via cap_session_mgr_get_current() and always landed here — even though
        the message was sent for ALIAS_B.
        """
        frames_a = self.frames_by_alias.get(self.ALIAS_A, [])
        self.assertEqual(
            len(frames_a), 0,
            f"ACK leaked to wrong session — NO_ACK flag not working. "
            f"Got {len(frames_a)} assistant frame(s) on ALIAS_A "
            f"(current session, should be 0): {frames_a}",
        )

    def test_no_early_ack_on_originating_session(self):
        """
        ALIAS_B (originating session) must not receive a very-short "ACK-like"
        assistant frame (< 30 chars) before any tool-progress frame.

        CLAW_IM_CHANNEL_FLAG_NO_ACK suppresses the early "working on it..." ACK
        that cap_im_local used to send before calling the LLM.  Such ACKs are
        short phrases sent within 0-200ms of the user message, before any tool
        calls.  Full LLM replies and tool-progress frames (🔧 prefix) are
        legitimate and are excluded from this check.

        NOTE: This test cannot be fully automated because:
          1. The LLM may respond within ACK_WINDOW on fast hardware.
          2. Distinguishing ACK vs real response requires heuristics.
        The core coverage is test_no_ack_leaked_to_current_session (ALIAS_A = 0).
        This test only catches obvious short pre-LLM ACK strings.
        """
        frames_b = self.frames_by_alias.get(self.ALIAS_B, [])
        # Only consider short frames that precede any tool-progress frame:
        # ACKs are typically < 30 chars. Real replies and tool-progress are longer.
        first_tool_idx = next(
            (i for i, f in enumerate(frames_b)
             if f.get("text", "").startswith("🔧")), len(frames_b)
        )
        pre_tool_frames = frames_b[:first_tool_idx]
        short_ack_frames = [f for f in pre_tool_frames
                            if len(f.get("text", "")) < 30]
        self.assertEqual(
            len(short_ack_frames), 0,
            f"Short ACK-like frame appeared on originating session before first "
            f"tool call — NO_ACK flag may not be suppressing ACK. "
            f"Got {len(short_ack_frames)} short frame(s): {short_ack_frames}",
        )

    def test_no_untagged_assistant_frames(self):
        """
        All assistant frames received in the observation window must carry a
        non-empty alias field.  An untagged frame means the routing layer lost
        track of which session to attribute the message to.
        """
        self.assertEqual(
            len(self.untagged_frames), 0,
            f"Untagged assistant frame(s) received — routing layer dropped alias "
            f"info. Frames: {self.untagged_frames}",
        )

    @classmethod
    def tearDownClass(cls):
        # Purge ring buffers for both aliases so the LLM response triggered by
        # PROBE_MSG (arrives ~20 s later) cannot bleed into subsequent test classes.
        for alias in (cls.ALIAS_A, cls.ALIAS_B):
            try:
                session_post({"action": "resume", "alias": alias})
                session_post({"action": "clear"})
            except Exception:
                pass
        time.sleep(_WS_TEARDOWN_SETTLE)


if __name__ == "__main__":
    unittest.main(verbosity=2)
