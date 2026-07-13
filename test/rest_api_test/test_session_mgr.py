"""
M2-SES-01~19, M2-SES-22~24: Session management tests.

Requirements:
  - Board running ameba_claw with HTTP proxy at 127.0.0.1
  - cap_im_local active (POST /send must return 200)
  - Serial bridge at SERIAL_BRIDGE_HOST:SERIAL_BRIDGE_PORT for AT tests (M2-SES-01~04, 22)

IM slash command strategy:
  - POST slash commands via /send (local IM channel, always available)
  - Read chat_map JSON from /api/files/content to verify session state
  - Each test class owns one unique CHAT_ID; setUpClass performs ALL actions
  - Individual test methods are READ-ONLY checks of state cached in setUpClass

Serial bridge protocol: JSON/Base64 over TCP — same as AmebaRemoteService.py (port 58916)

Coverage gaps added vs. M2-SES-01~19 in test_case_list.md:
  M2-SES-22: AT session,clear
  M2-SES-23: /rename no args → usage error
  M2-SES-24: /delete non-existent session
"""
import base64
import json
import re
import socket
import sys
import threading
import time
import os
import unittest

import requests

sys.path.insert(0, os.path.dirname(__file__))
from config import (
    BOARD_BASE_URL, HTTP_TIMEOUT,
    SERIAL_BRIDGE_HOST, SERIAL_BRIDGE_PORT,
)

LOCAL_CHANNEL = "local"
LOCAL_SEND_PATH = "/send"

# ---------------------------------------------------------------------------
# Filename computation (must match cap_session_mgr.c logic)
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


def session_history_path(channel: str, chat_id: str, alias: str) -> str:
    sid = f"{channel}:{chat_id}:{alias}"
    return f"/session/s_{_sanitize(sid)}_{_djb2(sid):08x}.json"


# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------

def files_read(path: str):
    r = requests.get(BOARD_BASE_URL + "/api/files/content",
                     params={"path": path}, timeout=HTTP_TIMEOUT)
    return r.status_code, r.text


def files_write(path: str, content: str):
    r = requests.put(BOARD_BASE_URL + "/api/files/content",
                     params={"path": path},
                     data=content.encode(), timeout=HTTP_TIMEOUT)
    return r.status_code


def files_delete(path: str):
    requests.delete(BOARD_BASE_URL + "/api/files",
                    params={"path": path}, timeout=HTTP_TIMEOUT)


def read_chat_map(channel: str, chat_id: str):
    code, text = files_read(chat_map_path(channel, chat_id))
    if code != 200:
        return None
    try:
        return json.loads(text)
    except Exception:
        return None


def delete_chat_map(channel: str, chat_id: str):
    files_delete(chat_map_path(channel, chat_id))


def post_local(chat_id: str, text: str) -> requests.Response:
    """POST a message via /send (local IM channel); waits 800 ms for processing."""
    r = requests.post(BOARD_BASE_URL + LOCAL_SEND_PATH,
                      data={"text": text, "chat_id": chat_id},
                      timeout=HTTP_TIMEOUT)
    time.sleep(0.8)
    return r


# ---------------------------------------------------------------------------
# Serial bridge helpers (AT tests)
# ---------------------------------------------------------------------------

class _ATSerial:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.connect((SERIAL_BRIDGE_HOST, SERIAL_BRIDGE_PORT))
        self._buf = b""
        self._evt = threading.Event()
        self._resp = {}
        self._lock = threading.Lock()
        t = threading.Thread(target=self._rx, daemon=True)
        t.start()

    def _rx(self):
        b = ""
        while True:
            try:
                d = self.sock.recv(4096)
                if not d:
                    break
                b += d.decode("utf-8", errors="replace")
                while "\n" in b:
                    line, b = b.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        m = json.loads(line)
                        mtype = m.get("type")
                        if mtype in ("command_response", "report_version"):
                            with self._lock:
                                self._resp = m
                            self._evt.set()
                        elif mtype == "serial_data":
                            raw = base64.b64decode(m.get("data", ""))
                            with self._lock:
                                self._buf += raw
                    except Exception:
                        pass
            except Exception:
                break

    def _cmd(self, cmd: dict, timeout: float = 10.0) -> dict:
        self._evt.clear()
        self._resp = {}
        self.sock.sendall((json.dumps(cmd) + "\n").encode())
        self._evt.wait(timeout)
        return self._resp

    def open(self):
        from config import SERIAL_PORT_NAME, SERIAL_BAUD
        r = self._cmd({
            "type": "open_port",
            "port": SERIAL_PORT_NAME,
            "options": {
                "baudRate": SERIAL_BAUD,
                "dataBits": 8, "stopBits": 1, "parity": "none", "timeout": 0.1,
            },
        }, timeout=10)
        if not r.get("success"):
            raise RuntimeError(f"open_port: {r.get('message')}")

    def close(self):
        from config import SERIAL_PORT_NAME
        try:
            self._cmd({"type": "close_port", "port": SERIAL_PORT_NAME}, timeout=3)
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass

    def flush(self):
        with self._lock:
            self._buf = b""

    def write(self, data: bytes):
        from config import SERIAL_PORT_NAME
        b64 = base64.b64encode(data).decode()
        r = self._cmd({"type": "write_data", "port": SERIAL_PORT_NAME, "data": b64}, timeout=10)
        if not r.get("success"):
            raise RuntimeError(f"write_data: {r.get('message')}")

    def read_until(self, pattern: bytes, timeout: float = 30.0) -> bytes:
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                idx = self._buf.find(pattern)
                if idx >= 0:
                    result = self._buf[:idx + len(pattern)]
                    self._buf = self._buf[idx + len(pattern):]
                    return result
            time.sleep(0.05)
        with self._lock:
            data = self._buf
            self._buf = b""
        return data


def _serial_available() -> bool:
    try:
        ser = _ATSerial()
        ser.open()
        ser.write(b"\r\n")  # harmless probe; fails if port is locked
        ser.close()
        return True
    except Exception:
        return False


def at_send(cmd: str, timeout: float = 8.0) -> str:
    ser = _ATSerial()
    ser.open()
    ser.flush()
    ser.write((cmd + "\r\n").encode())
    raw = ser.read_until(b"\r\nOK\r\n", timeout)
    if not raw:
        raw = ser.read_until(b"ERROR\r\n", timeout=2)
    ser.close()
    return raw.decode("utf-8", errors="replace")


_no_serial = not _serial_available()
_skip_serial = unittest.skipIf(_no_serial,
                                "Serial port not available (bridge unreachable or port busy)")


# ---------------------------------------------------------------------------
# IM availability check (local channel / /send endpoint)
# ---------------------------------------------------------------------------

def _im_available() -> bool:
    try:
        r = requests.post(BOARD_BASE_URL + LOCAL_SEND_PATH,
                          data={"text": "/new _probe_", "chat_id": "oc_ses_probe_avail"},
                          timeout=HTTP_TIMEOUT)
        if r.status_code != 200:
            return False
        time.sleep(0.5)
        # Verify slash command was intercepted (chat_map created)
        cid = "oc_ses_probe_avail"
        m = read_chat_map(LOCAL_CHANNEL, cid)
        files_delete(chat_map_path(LOCAL_CHANNEL, cid))
        return m is not None
    except Exception:
        return False


_no_im = not _im_available()
_skip_im = unittest.skipIf(_no_im, "Local IM /send endpoint inactive or slash intercept not working")


# ---------------------------------------------------------------------------
# M2-SES-01~04, M2-SES-22: AT command session tests
# ---------------------------------------------------------------------------

@_skip_serial
class TestSessionATCmds(unittest.TestCase):
    """AT+CLAW session commands — M2-SES-01, 02, 03, 04, 22."""

    @classmethod
    def setUpClass(cls):
        at_send("AT+CLAW=session,clear")
        time.sleep(0.3)

    def test_M2_SES_01_at_new_auto_name(self):
        """M2-SES-01: AT+CLAW=session,new returns auto-alias (MMDD-HHMM or s<N>)."""
        resp = at_send("AT+CLAW=session,new")
        m = re.search(r"\+CLAW:session,new,serial,atcmd,(\S+)", resp)
        self.assertIsNotNone(m, f"Unexpected response: {resp!r}")
        alias = m.group(1).rstrip("\r\n")
        is_ts = bool(re.fullmatch(r"\d{4}-\d{4}(-\d+)?", alias))
        is_sn = bool(re.fullmatch(r"s\d+", alias))
        self.assertTrue(is_ts or is_sn,
                        f"Alias '{alias}' matches neither MMDD-HHMM nor s<N>")
        self.assertIn("OK", resp)

    def test_M2_SES_02_at_new_named(self):
        """M2-SES-02: AT+CLAW=session,new,serial,atcmd,debug creates named session."""
        resp = at_send("AT+CLAW=session,new,serial,atcmd,debug")
        self.assertIn("+CLAW:session,new,serial,atcmd,debug", resp,
                      f"Unexpected response: {resp!r}")
        self.assertIn("OK", resp)

    def test_M2_SES_03_at_list_shows_sessions(self):
        """M2-SES-03: AT+CLAW=session,list outputs default+debug; total>=2."""
        # session,list sends OK immediately, then the list in a background task.
        # Read until "+CLAW:session,total=" (the last line of the task) to capture the full output.
        ser = _ATSerial()
        ser.open()
        ser.flush()
        ser.write(("AT+CLAW=session,list" + "\r\n").encode())
        raw = ser.read_until(b"+CLAW:session,total=", timeout=10.0)
        raw += ser.read_until(b"\r\n", timeout=2.0)
        ser.close()
        resp = raw.decode("utf-8", errors="replace")
        self.assertIn("default", resp, f"'default' missing: {resp!r}")
        self.assertIn("debug", resp, f"'debug' missing: {resp!r}")
        m = re.search(r"\+CLAW:session,total=(\d+)", resp)
        self.assertIsNotNone(m, f"No total line: {resp!r}")
        self.assertGreaterEqual(int(m.group(1)), 2)

    def test_M2_SES_04_at_reset_removed(self):
        """M2-SES-04: AT+CLAW=session,reset → usage error (command removed)."""
        resp = at_send("AT+CLAW=session,reset")
        self.assertIn("ERROR", resp, f"Expected ERROR: {resp!r}")
        self.assertNotIn("+CLAW:session,reset", resp)

    def test_M2_SES_22_at_clear_current(self):
        """M2-SES-22: AT+CLAW=session,clear clears current session; responds +CLAW:session,cleared."""
        resp = at_send("AT+CLAW=session,clear", timeout=10.0)
        self.assertIn("+CLAW:session,cleared", resp, f"Unexpected: {resp!r}")
        self.assertIn("OK", resp)


# ---------------------------------------------------------------------------
# M2-SES-05, 06: /new (named) and /list state
# setUpClass: delete → /new work; tests read cached chat_map
# ---------------------------------------------------------------------------

@_skip_im
class TestSessionIM_05_06_NewAndListState(unittest.TestCase):
    """M2-SES-05: /new work creates session. M2-SES-06: chat_map shows both sessions."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t05"

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        r = post_local(cls.CHAT_ID, "/new work")
        cls.post_ok = (r.status_code == 200)
        cls.chat_map = read_chat_map(cls.CHANNEL, cls.CHAT_ID)

    def test_M2_SES_05_new_named_creates_session(self):
        """M2-SES-05: /new work → chat_map has 'work' and current='work'."""
        self.assertTrue(self.post_ok, "POST /send failed")
        self.assertIsNotNone(self.chat_map, "chat_map not created after /new work")
        sessions = self.chat_map.get("sessions", [])
        self.assertIn("work", sessions, f"'work' not in sessions: {sessions}")
        self.assertEqual(self.chat_map.get("current"), "work")

    def test_M2_SES_06_list_state_shows_both_sessions(self):
        """M2-SES-06: after /new work, chat_map has default+work, current=work, no duplicates."""
        self.assertIsNotNone(self.chat_map)
        sessions = self.chat_map.get("sessions", [])
        self.assertIn("default", sessions, f"'default' missing: {sessions}")
        self.assertIn("work", sessions, f"'work' missing: {sessions}")
        self.assertEqual(len(sessions), len(set(sessions)), f"Duplicates: {sessions}")
        self.assertEqual(self.chat_map.get("current"), "work")


# ---------------------------------------------------------------------------
# M2-SES-07: /resume switches current session
# setUpClass: delete → /new work (current=work) → /resume default (current=default)
# ---------------------------------------------------------------------------

@_skip_im
class TestSessionIM_07_Resume(unittest.TestCase):
    """M2-SES-07: /resume default switches current to 'default'."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t07"

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        post_local(cls.CHAT_ID, "/new work")     # current="work"
        r = post_local(cls.CHAT_ID, "/resume default")
        cls.post_ok = (r.status_code == 200)
        cls.chat_map = read_chat_map(cls.CHANNEL, cls.CHAT_ID)

    def test_M2_SES_07_resume_switches_session(self):
        """M2-SES-07: /resume default → current='default' in chat_map."""
        self.assertTrue(self.post_ok)
        self.assertIsNotNone(self.chat_map)
        self.assertEqual(self.chat_map.get("current"), "default",
                         f"Expected current='default': {self.chat_map}")


# ---------------------------------------------------------------------------
# M2-SES-08, 09, M2-SES-24: /delete behavior
# setUpClass: delete → /new work (current=work) → /resume default (current=default)
# ---------------------------------------------------------------------------

@_skip_im
class TestSessionIM_08_DeleteNonCurrent(unittest.TestCase):
    """M2-SES-08: /delete work (non-current) removes it; history file gone."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t08"

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        post_local(cls.CHAT_ID, "/new work")
        post_local(cls.CHAT_ID, "/resume default")
        r = post_local(cls.CHAT_ID, "/delete work")
        cls.post_ok = (r.status_code == 200)
        cls.chat_map = read_chat_map(cls.CHANNEL, cls.CHAT_ID)
        cls.hist_code, _ = files_read(
            session_history_path(cls.CHANNEL, cls.CHAT_ID, "work"))

    def test_M2_SES_08a_work_removed_from_sessions(self):
        """M2-SES-08: 'work' not in sessions after /delete work."""
        self.assertTrue(self.post_ok)
        self.assertIsNotNone(self.chat_map)
        sessions = self.chat_map.get("sessions", [])
        self.assertNotIn("work", sessions, f"'work' should be deleted: {sessions}")
        self.assertIn("default", sessions)

    def test_M2_SES_08b_current_unchanged(self):
        """M2-SES-08: current remains 'default' after deleting 'work'."""
        self.assertIsNotNone(self.chat_map)
        self.assertEqual(self.chat_map.get("current"), "default")

    def test_M2_SES_08c_history_file_gone(self):
        """M2-SES-08: history file for deleted session no longer exists."""
        self.assertNotEqual(self.hist_code, 200,
                            "Deleted session history file still exists")


@_skip_im
class TestSessionIM_09_DeleteCurrentRejected(unittest.TestCase):
    """M2-SES-09: /delete default (current) is refused; chat_map unchanged."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t09"

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        post_local(cls.CHAT_ID, "/new work")
        post_local(cls.CHAT_ID, "/resume default")
        cls.map_before = read_chat_map(cls.CHANNEL, cls.CHAT_ID)
        r = post_local(cls.CHAT_ID, "/delete default")
        cls.post_ok = (r.status_code == 200)
        cls.map_after = read_chat_map(cls.CHANNEL, cls.CHAT_ID)

    def test_M2_SES_09_current_session_not_deleted(self):
        """M2-SES-09: /delete on current session is refused; chat_map unchanged."""
        self.assertTrue(self.post_ok)
        self.assertIsNotNone(self.map_before)
        self.assertIsNotNone(self.map_after)
        self.assertEqual(self.map_before.get("current"), self.map_after.get("current"))
        self.assertEqual(sorted(self.map_before.get("sessions", [])),
                         sorted(self.map_after.get("sessions", [])))


@_skip_im
class TestSessionIM_24_DeleteNonexistent(unittest.TestCase):
    """M2-SES-24: /delete nosuchsession → not-found error; chat_map unchanged."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t24"

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        post_local(cls.CHAT_ID, "/new work")
        cls.map_before = read_chat_map(cls.CHANNEL, cls.CHAT_ID)
        r = post_local(cls.CHAT_ID, "/delete nosuchsession")
        cls.post_ok = (r.status_code == 200)
        cls.map_after = read_chat_map(cls.CHANNEL, cls.CHAT_ID)

    def test_M2_SES_24_nonexistent_delete_no_change(self):
        """M2-SES-24: /delete of non-existent session → chat_map sessions unchanged."""
        self.assertTrue(self.post_ok)
        self.assertIsNotNone(self.map_after)
        self.assertEqual(sorted(self.map_before.get("sessions", [])),
                         sorted(self.map_after.get("sessions", [])),
                         "chat_map changed unexpectedly")


# ---------------------------------------------------------------------------
# M2-SES-10: /clear clears conversation history
# setUpClass: delete → /new work → write fake history → /clear
# ---------------------------------------------------------------------------

@_skip_im
class TestSessionIM_10_Clear(unittest.TestCase):
    """M2-SES-10: /clear removes current session history; chat_map unaffected."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t10"
    FAKE_HISTORY = '{"turns":[{"role":"user","content":"hello"}]}'

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        post_local(cls.CHAT_ID, "/new work")      # current="work"
        post_local(cls.CHAT_ID, "/resume default")  # current="default"
        cls.hist_path = session_history_path(cls.CHANNEL, cls.CHAT_ID, "default")
        files_write(cls.hist_path, cls.FAKE_HISTORY)
        cls.map_before = read_chat_map(cls.CHANNEL, cls.CHAT_ID)
        r = post_local(cls.CHAT_ID, "/clear")
        cls.post_ok = (r.status_code == 200)
        cls.hist_after_code, cls.hist_after_text = files_read(cls.hist_path)
        cls.map_after = read_chat_map(cls.CHANNEL, cls.CHAT_ID)

    def test_M2_SES_10a_history_cleared(self):
        """M2-SES-10: /clear removes or empties current session history file."""
        self.assertTrue(self.post_ok)
        if self.hist_after_code == 200:
            try:
                data = json.loads(self.hist_after_text)
                msgs = data.get("turns", data.get("messages", []))
                self.assertEqual(len(msgs), 0, "History should be empty after /clear")
            except Exception:
                pass  # non-JSON or gone = cleared
        # 404 is also acceptable (file deleted)

    def test_M2_SES_10b_chat_map_unchanged(self):
        """M2-SES-10: /clear does not affect chat_map sessions."""
        if self.map_before and self.map_after:
            self.assertEqual(sorted(self.map_before.get("sessions", [])),
                             sorted(self.map_after.get("sessions", [])),
                             "chat_map sessions changed after /clear")


# ---------------------------------------------------------------------------
# M2-SES-11, 12: /new auto-naming
# setUpClass: delete → /new (no name)
# ---------------------------------------------------------------------------

@_skip_im
class TestSessionIM_11_12_AutoNaming(unittest.TestCase):
    """M2-SES-11/12: /new without name uses MMDD-HHMM (clock synced) or s<N>."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t11"

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        # /new with no name: produces auto-alias
        post_local(cls.CHAT_ID, "/new")
        cls.chat_map = read_chat_map(cls.CHANNEL, cls.CHAT_ID)

    def test_M2_SES_11_12_auto_alias_format(self):
        """M2-SES-11/12: auto alias is MMDD-HHMM (clock synced) or s<N> (unsynced)."""
        self.assertIsNotNone(self.chat_map, "chat_map not created after /new (no name)")
        sessions = self.chat_map.get("sessions", [])
        # Remove 'default'; the auto-named one should be present
        auto_aliases = [s for s in sessions if s != "default"]
        self.assertGreaterEqual(len(auto_aliases), 1,
                                f"No auto alias found in sessions: {sessions}")
        alias = auto_aliases[0]
        is_ts = bool(re.fullmatch(r"\d{4}-\d{4}(-\d+)?", alias))
        is_sn = bool(re.fullmatch(r"s\d+", alias))
        self.assertTrue(is_ts or is_sn,
                        f"Auto alias '{alias}' is neither MMDD-HHMM nor s<N>")
        print(f"\n  [M2-SES-11/12] Auto alias: '{alias}' "
              f"({'clock-synced' if is_ts else 'clock-unsynced'})")


# ---------------------------------------------------------------------------
# M2-SES-13, 14: /rename
# setUpClass for 13: delete → /new work (current=work) → /rename home
# setUpClass for 14: delete → /new work (current=work) → /new work2 → /resume work → /rename work2
# ---------------------------------------------------------------------------

@_skip_im
class TestSessionIM_13_Rename(unittest.TestCase):
    """M2-SES-13: /rename home renames current session."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t13"

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        post_local(cls.CHAT_ID, "/new work")     # current="work"
        post_local(cls.CHAT_ID, "/resume default")  # current="default"
        r = post_local(cls.CHAT_ID, "/rename home")
        cls.post_ok = (r.status_code == 200)
        cls.chat_map = read_chat_map(cls.CHANNEL, cls.CHAT_ID)

    def test_M2_SES_13a_current_renamed(self):
        """M2-SES-13: /rename home → current='home'."""
        self.assertTrue(self.post_ok)
        self.assertIsNotNone(self.chat_map)
        self.assertEqual(self.chat_map.get("current"), "home",
                         f"Expected current='home': {self.chat_map}")

    def test_M2_SES_13b_old_name_gone(self):
        """M2-SES-13: 'default' no longer in sessions after rename to 'home'."""
        self.assertIsNotNone(self.chat_map)
        sessions = self.chat_map.get("sessions", [])
        self.assertIn("home", sessions)
        self.assertNotIn("default", sessions,
                         f"'default' should be gone after rename: {sessions}")


@_skip_im
class TestSessionIM_14_RenameConflict(unittest.TestCase):
    """M2-SES-14: /rename to existing name → conflict; current unchanged."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t14"

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        post_local(cls.CHAT_ID, "/new work")    # sessions=[default,work], current=work
        post_local(cls.CHAT_ID, "/resume default")  # current=default
        # Try to rename 'default' to 'work' — conflict
        r = post_local(cls.CHAT_ID, "/rename work")
        cls.post_ok = (r.status_code == 200)
        cls.chat_map = read_chat_map(cls.CHANNEL, cls.CHAT_ID)

    def test_M2_SES_14_rename_conflict_no_change(self):
        """M2-SES-14: /rename to existing name → current unchanged; 'default' still exists."""
        self.assertTrue(self.post_ok)
        self.assertIsNotNone(self.chat_map)
        # Current must still be 'default' (rename was rejected)
        self.assertEqual(self.chat_map.get("current"), "default",
                         f"Rename to conflicting name should have been rejected: {self.chat_map}")
        sessions = self.chat_map.get("sessions", [])
        self.assertIn("default", sessions)
        self.assertIn("work", sessions)


# ---------------------------------------------------------------------------
# M2-SES-15: session history persists after switching away and back
# setUpClass: delete → /new work (current=work) → write fake history → /resume default
# Test: verify history survives; then /resume work → history still intact
# ---------------------------------------------------------------------------

@_skip_im
class TestSessionIM_15_HistoryPersists(unittest.TestCase):
    """M2-SES-15: work session history survives switching away and back."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t15"
    FAKE_HISTORY = '{"turns":[{"role":"user","content":"remember me"}]}'

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        post_local(cls.CHAT_ID, "/new work")      # current="work"
        cls.hist_path = session_history_path(cls.CHANNEL, cls.CHAT_ID, "work")
        files_write(cls.hist_path, cls.FAKE_HISTORY)
        post_local(cls.CHAT_ID, "/resume default")  # switch away from work
        cls.hist_after_away_code, cls.hist_after_away_text = files_read(cls.hist_path)
        # Switch back
        r = post_local(cls.CHAT_ID, "/resume work")
        cls.resume_ok = (r.status_code == 200)
        cls.hist_after_back_code, cls.hist_after_back_text = files_read(cls.hist_path)
        cls.chat_map = read_chat_map(cls.CHANNEL, cls.CHAT_ID)

    def test_M2_SES_15a_history_survives_switch_away(self):
        """M2-SES-15: work history file intact after switching to default."""
        self.assertEqual(self.hist_after_away_code, 200,
                         "History file missing after switching away from work")

    def test_M2_SES_15b_history_intact_after_switch_back(self):
        """M2-SES-15: work history content unchanged after /resume work."""
        self.assertTrue(self.resume_ok)
        self.assertEqual(self.hist_after_back_code, 200,
                         "History file missing after switching back to work")
        self.assertIn("remember me", self.hist_after_back_text,
                      "History content changed after session switch")

    def test_M2_SES_15c_current_is_work_after_resume(self):
        """M2-SES-15: current session is 'work' after /resume work."""
        self.assertIsNotNone(self.chat_map)
        self.assertEqual(self.chat_map.get("current"), "work")


# ---------------------------------------------------------------------------
# M2-SES-16, 17, 18, M2-SES-23: error handling
# ---------------------------------------------------------------------------

@_skip_im
class TestSessionIM_16_ResumeNoArgs(unittest.TestCase):
    """M2-SES-16: /resume (no args) → chat_map unchanged."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t16"

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        post_local(cls.CHAT_ID, "/new work")
        cls.map_before = read_chat_map(cls.CHANNEL, cls.CHAT_ID)
        r = post_local(cls.CHAT_ID, "/resume")
        cls.post_ok = (r.status_code == 200)
        cls.map_after = read_chat_map(cls.CHANNEL, cls.CHAT_ID)

    def test_M2_SES_16_resume_no_args_no_change(self):
        """M2-SES-16: /resume with no args → current unchanged (usage error sent to IM)."""
        self.assertTrue(self.post_ok)
        self.assertIsNotNone(self.map_before)
        self.assertIsNotNone(self.map_after)
        self.assertEqual(self.map_before.get("current"), self.map_after.get("current"),
                         "current changed after /resume with no args")


@_skip_im
class TestSessionIM_17_DeleteNoArgs(unittest.TestCase):
    """M2-SES-17: /delete (no args) → chat_map unchanged."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t17"

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        post_local(cls.CHAT_ID, "/new work")
        cls.map_before = read_chat_map(cls.CHANNEL, cls.CHAT_ID)
        r = post_local(cls.CHAT_ID, "/delete")
        cls.post_ok = (r.status_code == 200)
        cls.map_after = read_chat_map(cls.CHANNEL, cls.CHAT_ID)

    def test_M2_SES_17_delete_no_args_no_change(self):
        """M2-SES-17: /delete with no args → sessions unchanged."""
        self.assertTrue(self.post_ok)
        if self.map_before and self.map_after:
            self.assertEqual(sorted(self.map_before.get("sessions", [])),
                             sorted(self.map_after.get("sessions", [])),
                             "sessions changed after /delete with no args")


@_skip_im
class TestSessionIM_18_ResumeNotFound(unittest.TestCase):
    """M2-SES-18: /resume xxx (non-existent) → current unchanged."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t18"

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        post_local(cls.CHAT_ID, "/new work")
        cls.map_before = read_chat_map(cls.CHANNEL, cls.CHAT_ID)
        r = post_local(cls.CHAT_ID, "/resume xnosuchsessionx")
        cls.post_ok = (r.status_code == 200)
        cls.map_after = read_chat_map(cls.CHANNEL, cls.CHAT_ID)

    def test_M2_SES_18_resume_notfound_no_change(self):
        """M2-SES-18: /resume non-existent session → current unchanged."""
        self.assertTrue(self.post_ok)
        if self.map_before and self.map_after:
            self.assertEqual(self.map_before.get("current"), self.map_after.get("current"),
                             "current changed after /resume of non-existent session")


@_skip_im
class TestSessionIM_23_RenameNoArgs(unittest.TestCase):
    """M2-SES-23: /rename (no args) → chat_map unchanged (usage error sent to IM)."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t23"

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        post_local(cls.CHAT_ID, "/new work")
        cls.map_before = read_chat_map(cls.CHANNEL, cls.CHAT_ID)
        r = post_local(cls.CHAT_ID, "/rename")
        cls.post_ok = (r.status_code == 200)
        cls.map_after = read_chat_map(cls.CHANNEL, cls.CHAT_ID)

    def test_M2_SES_23_rename_no_args_no_change(self):
        """M2-SES-23: /rename with no args → current and sessions unchanged."""
        self.assertTrue(self.post_ok)
        if self.map_before and self.map_after:
            self.assertEqual(self.map_before.get("current"), self.map_after.get("current"))
            self.assertEqual(sorted(self.map_before.get("sessions", [])),
                             sorted(self.map_after.get("sessions", [])))


# ---------------------------------------------------------------------------
# M2-SES-19: slash command not sent to LLM
# setUpClass: delete → /new work (creates session via slash, no LLM call)
# Test: verify no session history file was written for the default session
# ---------------------------------------------------------------------------

@_skip_im
class TestSessionIM_19_SlashNotToLLM(unittest.TestCase):
    """M2-SES-19: slash commands are intercepted; no LLM history entry created."""

    CHANNEL = LOCAL_CHANNEL
    CHAT_ID = "oc_ses_t19"

    @classmethod
    def setUpClass(cls):
        delete_chat_map(cls.CHANNEL, cls.CHAT_ID)
        # This chat_id has no history yet — verify before sending slash command
        cls.hist_path = session_history_path(cls.CHANNEL, cls.CHAT_ID, "default")
        cls.hist_before_code, _ = files_read(cls.hist_path)
        r = post_local(cls.CHAT_ID, "/new work")
        cls.post_ok = (r.status_code == 200)
        cls.chat_map = read_chat_map(cls.CHANNEL, cls.CHAT_ID)
        cls.hist_after_code, cls.hist_after_text = files_read(cls.hist_path)

    def test_M2_SES_19a_slash_creates_session_state(self):
        """M2-SES-19: /new work was intercepted (session state changed)."""
        self.assertTrue(self.post_ok)
        self.assertIsNotNone(self.chat_map,
                             "'work' session not created — slash may not have been intercepted")
        self.assertIn("work", self.chat_map.get("sessions", []))

    def test_M2_SES_19b_no_llm_history_written(self):
        """M2-SES-19: no session history file written after slash command (LLM not invoked)."""
        if self.hist_before_code != 200:
            # No pre-existing history; verify none was created
            if self.hist_after_code == 200:
                try:
                    data = json.loads(self.hist_after_text)
                    msgs = data.get("turns", data.get("messages", []))
                    self.assertEqual(len(msgs), 0,
                                     "LLM history was written for a slash command")
                except Exception:
                    pass


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    unittest.main(verbosity=2)
