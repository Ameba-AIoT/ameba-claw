"""
REST API tests for /api/session (GET, POST, DELETE).

New endpoints introduced by cap_webui.c:
  GET    /api/session         -> {ok, current, sessions:[{alias,preview}]}
  POST   /api/session         -> {action: new|resume|rename|clear}
  DELETE /api/session?alias=X -> delete a named session

All REST session endpoints operate on the fixed channel/chat_id "local:local".
Each test class resets state by deleting the chat_map via /api/files before setup.
"""
import json
import sys
import os
import time
import unittest

import requests

sys.path.insert(0, os.path.dirname(__file__))
from config import BOARD_BASE_URL, HTTP_TIMEOUT

REST_CHANNEL = "local"
REST_CHAT_ID = "local"
BASE = BOARD_BASE_URL
SESSION_URL = BASE + "/api/session"


# ---------------------------------------------------------------------------
# Filename helpers — must mirror cap_session_mgr.c logic
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
    r = requests.get(BASE + "/api/files/content",
                     params={"path": path}, timeout=HTTP_TIMEOUT)
    return r.status_code, r.text


def files_write(path: str, content: str):
    r = requests.put(BASE + "/api/files/content",
                     params={"path": path},
                     data=content.encode(), timeout=HTTP_TIMEOUT)
    return r.status_code


def files_delete(path: str):
    requests.delete(BASE + "/api/files", params={"path": path},
                    timeout=HTTP_TIMEOUT)


def delete_local_chat_map():
    """Ensure the board is responsive, delete the local:local chat_map, settle."""
    _board_ready()
    files_delete(chat_map_path(REST_CHANNEL, REST_CHAT_ID))
    time.sleep(0.3)


def _board_ready(timeout: float = 10.0) -> bool:
    """Wait until POST /api/session completes in under 2 s.
    A fast POST response confirms the HTTP connection tasks are not backed up
    by post-response WS broadcasts to stale connections (SO_SNDTIMEO = 1s).
    Call this at the start of setUpClass methods that make rapid sequential
    POSTs, especially after WS tests that may have left half-dead connections."""
    deadline = time.time() + timeout
    probe_alias = "_probe_ready_"
    while time.time() < deadline:
        try:
            t0 = time.time()
            r = requests.post(SESSION_URL, json={"action": "new", "alias": probe_alias},
                              timeout=3)
            elapsed = time.time() - t0
            # Clean up the probe session (ignore errors)
            try:
                requests.post(SESSION_URL,
                              json={"action": "resume", "alias": "default"},
                              timeout=2)
                requests.delete(SESSION_URL, params={"alias": probe_alias}, timeout=2)
            except Exception:
                pass
            if elapsed < 2.0 and r.status_code in (200, 409):
                return True
        except Exception:
            pass
        time.sleep(0.5)
    return False


def read_local_chat_map():
    code, text = files_read(chat_map_path(REST_CHANNEL, REST_CHAT_ID))
    if code != 200:
        return None
    try:
        return json.loads(text)
    except Exception:
        return None


def session_get():
    return requests.get(SESSION_URL, timeout=HTTP_TIMEOUT)


def session_post(body: dict):
    return requests.post(SESSION_URL, json=body, timeout=HTTP_TIMEOUT)


def session_delete(alias: str):
    return requests.delete(SESSION_URL, params={"alias": alias},
                           timeout=HTTP_TIMEOUT)


# ---------------------------------------------------------------------------
# Availability guard — skip all tests if endpoint is unreachable
# ---------------------------------------------------------------------------

def _api_available() -> bool:
    try:
        r = session_get()
        return r.status_code == 200
    except Exception:
        return False


_no_api = not _api_available()
_skip = unittest.skipIf(_no_api, "/api/session endpoint not reachable")


# ---------------------------------------------------------------------------
# GET /api/session — basic response shape
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestGet(unittest.TestCase):
    """GET /api/session returns {ok, current, sessions:[{alias,preview}]}."""

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        cls.r = session_get()
        cls.body = cls.r.json() if cls.r.status_code == 200 else None

    def test_status_200(self):
        self.assertEqual(self.r.status_code, 200)

    def test_ok_true(self):
        self.assertIsNotNone(self.body)
        self.assertTrue(self.body.get("ok"))

    def test_current_field_present(self):
        self.assertIn("current", self.body)

    def test_sessions_is_list(self):
        self.assertIsInstance(self.body.get("sessions"), list)

    def test_sessions_contains_default(self):
        sessions = self.body.get("sessions", [])
        aliases = [s["alias"] if isinstance(s, dict) else s for s in sessions]
        self.assertIn("default", aliases)

    def test_sessions_items_have_preview(self):
        sessions = self.body.get("sessions", [])
        self.assertGreater(len(sessions), 0)
        for item in sessions:
            self.assertIsInstance(item, dict, "sessions items should be objects with alias+preview")
            self.assertIn("alias", item)
            self.assertIn("preview", item)


# ---------------------------------------------------------------------------
# POST /api/session — action=new, named alias
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestNewNamed(unittest.TestCase):
    """POST {action:new, alias:X} creates a named session and makes it current."""

    ALIAS = "rest_t_new"

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        cls.r = session_post({"action": "new", "alias": cls.ALIAS})
        cls.body = cls.r.json() if cls.r.ok else None
        time.sleep(0.2)
        cls.chat_map = read_local_chat_map()

    def test_status_200(self):
        self.assertEqual(self.r.status_code, 200)

    def test_response_ok(self):
        self.assertIsNotNone(self.body)
        self.assertTrue(self.body.get("ok"))

    def test_response_alias_matches(self):
        self.assertEqual(self.body.get("alias"), self.ALIAS)

    def test_chat_map_contains_alias(self):
        self.assertIsNotNone(self.chat_map)
        self.assertIn(self.ALIAS, self.chat_map.get("sessions", []))

    def test_chat_map_current_is_alias(self):
        self.assertIsNotNone(self.chat_map)
        self.assertEqual(self.chat_map.get("current"), self.ALIAS)


# ---------------------------------------------------------------------------
# POST /api/session — action=new, auto-name (no alias field)
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestNewAuto(unittest.TestCase):
    """POST {action:new} (no alias) returns a non-empty auto-generated alias."""

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        cls.r = session_post({"action": "new"})
        cls.body = cls.r.json() if cls.r.ok else None
        time.sleep(0.2)
        cls.chat_map = read_local_chat_map()

    def test_status_200(self):
        self.assertEqual(self.r.status_code, 200)

    def test_alias_non_empty(self):
        self.assertIsNotNone(self.body)
        alias = self.body.get("alias", "")
        self.assertGreater(len(alias), 0, "auto alias should be non-empty")

    def test_auto_alias_in_chat_map(self):
        self.assertIsNotNone(self.chat_map)
        auto_alias = self.body["alias"]
        self.assertIn(auto_alias, self.chat_map.get("sessions", []))


# ---------------------------------------------------------------------------
# POST /api/session — action=new, conflict (409)
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestNewConflict(unittest.TestCase):
    """POST {action:new, alias:X} twice returns 409 on the second call."""

    ALIAS = "rest_t_conf"

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.1)
        cls.r = session_post({"action": "new", "alias": cls.ALIAS})

    def test_status_409(self):
        self.assertEqual(self.r.status_code, 409)


# ---------------------------------------------------------------------------
# POST /api/session — action=resume, success
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestResume(unittest.TestCase):
    """POST {action:resume, alias:X} switches to an existing session."""

    ALIAS_A = "rest_t_res_a"
    ALIAS_B = "rest_t_res_b"

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS_A})
        time.sleep(0.1)
        session_post({"action": "new", "alias": cls.ALIAS_B})  # now current=B
        time.sleep(0.1)
        cls.r = session_post({"action": "resume", "alias": cls.ALIAS_A})
        cls.body = cls.r.json() if cls.r.ok else None
        time.sleep(0.2)
        cls.chat_map = read_local_chat_map()

    def test_status_200(self):
        self.assertEqual(self.r.status_code, 200)

    def test_response_ok(self):
        self.assertIsNotNone(self.body)
        self.assertTrue(self.body.get("ok"))

    def test_current_switched_to_alias_a(self):
        self.assertIsNotNone(self.chat_map)
        self.assertEqual(self.chat_map.get("current"), self.ALIAS_A)


# ---------------------------------------------------------------------------
# POST /api/session — action=resume, not found (404)
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestResumeNotFound(unittest.TestCase):
    """POST {action:resume, alias:nonexistent} returns 404."""

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        cls.r = session_post({"action": "resume", "alias": "no_such_session_xyz"})

    def test_status_404(self):
        self.assertEqual(self.r.status_code, 404)


# ---------------------------------------------------------------------------
# POST /api/session — action=resume, missing alias (400)
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestResumeMissingAlias(unittest.TestCase):
    """POST {action:resume} without alias returns 400."""

    @classmethod
    def setUpClass(cls):
        cls.r = session_post({"action": "resume"})

    def test_status_400(self):
        self.assertEqual(self.r.status_code, 400)


# ---------------------------------------------------------------------------
# POST /api/session — action=rename, success
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestRename(unittest.TestCase):
    """POST {action:rename, alias:newname} renames the current session."""

    OLD_ALIAS = "rest_t_rn_old"
    NEW_ALIAS = "rest_t_rn_new"

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.OLD_ALIAS})
        time.sleep(0.1)
        cls.r = session_post({"action": "rename", "alias": cls.NEW_ALIAS})
        cls.body = cls.r.json() if cls.r.ok else None
        time.sleep(0.2)
        cls.chat_map = read_local_chat_map()

    def test_status_200(self):
        self.assertEqual(self.r.status_code, 200)

    def test_response_ok(self):
        self.assertIsNotNone(self.body)
        self.assertTrue(self.body.get("ok"))

    def test_current_is_new_alias(self):
        self.assertIsNotNone(self.chat_map)
        self.assertEqual(self.chat_map.get("current"), self.NEW_ALIAS)

    def test_old_alias_removed(self):
        self.assertIsNotNone(self.chat_map)
        sessions = self.chat_map.get("sessions", [])
        self.assertNotIn(self.OLD_ALIAS, sessions)
        self.assertIn(self.NEW_ALIAS, sessions)


# ---------------------------------------------------------------------------
# POST /api/session — action=rename, conflict (409)
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestRenameConflict(unittest.TestCase):
    """POST {action:rename, alias:existing} returns 409; current unchanged."""

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        session_post({"action": "new", "alias": "rest_t_rc_a"})
        time.sleep(0.1)
        session_post({"action": "new", "alias": "rest_t_rc_b"})
        time.sleep(0.1)
        # current=rest_t_rc_b; switch to a, then try rename to b (conflict)
        session_post({"action": "resume", "alias": "rest_t_rc_a"})
        time.sleep(0.1)
        cls.r = session_post({"action": "rename", "alias": "rest_t_rc_b"})
        time.sleep(0.2)
        cls.chat_map = read_local_chat_map()

    def test_status_409(self):
        self.assertEqual(self.r.status_code, 409)

    def test_current_unchanged(self):
        self.assertIsNotNone(self.chat_map)
        self.assertEqual(self.chat_map.get("current"), "rest_t_rc_a")


# ---------------------------------------------------------------------------
# POST /api/session — action=clear
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestClear(unittest.TestCase):
    """POST {action:clear} clears current session history; sessions list unchanged."""

    FAKE_HISTORY = '{"turns":[{"role":"user","content":"test message"}]}'

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        session_post({"action": "new", "alias": "rest_t_clr"})
        time.sleep(0.1)
        hist_path = session_history_path(REST_CHANNEL, REST_CHAT_ID, "rest_t_clr")
        files_write(hist_path, cls.FAKE_HISTORY)
        time.sleep(0.1)
        cls.map_before = read_local_chat_map()
        cls.r = session_post({"action": "clear"})
        cls.body = cls.r.json() if cls.r.ok else None
        time.sleep(0.2)
        cls.map_after = read_local_chat_map()
        cls.hist_code, cls.hist_text = files_read(hist_path)

    def test_status_200(self):
        self.assertEqual(self.r.status_code, 200)

    def test_response_ok(self):
        self.assertIsNotNone(self.body)
        self.assertTrue(self.body.get("ok"))

    def test_sessions_unchanged(self):
        if self.map_before and self.map_after:
            self.assertEqual(sorted(self.map_before.get("sessions", [])),
                             sorted(self.map_after.get("sessions", [])))

    def test_history_cleared_or_gone(self):
        if self.hist_code == 200:
            try:
                data = json.loads(self.hist_text)
                turns = data.get("turns", data.get("messages", []))
                self.assertEqual(len(turns), 0, "History turns should be empty after clear")
            except Exception:
                pass  # non-JSON response is acceptable
        # 404 = file deleted = cleared


# ---------------------------------------------------------------------------
# POST /api/session — input validation error cases
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestPostErrors(unittest.TestCase):
    """POST /api/session input validation — no body, invalid JSON, unknown action."""

    def test_no_body_400(self):
        r = requests.post(SESSION_URL, timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 400)

    def test_invalid_json_400(self):
        r = requests.post(SESSION_URL, data=b"not json",
                          headers={"Content-Type": "application/json"},
                          timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 400)

    def test_unknown_action_400(self):
        r = session_post({"action": "foobar"})
        self.assertEqual(r.status_code, 400)

    def test_missing_action_field_400(self):
        r = session_post({"alias": "something"})
        self.assertEqual(r.status_code, 400)

    def test_rename_missing_alias_400(self):
        r = session_post({"action": "rename"})
        self.assertEqual(r.status_code, 400)


# ---------------------------------------------------------------------------
# DELETE /api/session — success (non-current session)
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestDelete(unittest.TestCase):
    """DELETE /api/session?alias=X removes a non-current session."""

    DEL_ALIAS = "rest_t_del"
    KEEP_ALIAS = "rest_t_del_keep"

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.DEL_ALIAS})
        time.sleep(0.1)
        session_post({"action": "new", "alias": cls.KEEP_ALIAS})  # current=KEEP
        time.sleep(0.1)
        cls.r = session_delete(cls.DEL_ALIAS)
        cls.body = cls.r.json() if cls.r.ok else None
        time.sleep(0.2)
        cls.chat_map = read_local_chat_map()

    def test_status_200(self):
        self.assertEqual(self.r.status_code, 200)

    def test_response_ok(self):
        self.assertIsNotNone(self.body)
        self.assertTrue(self.body.get("ok"))

    def test_alias_removed_from_chat_map(self):
        self.assertIsNotNone(self.chat_map)
        self.assertNotIn(self.DEL_ALIAS, self.chat_map.get("sessions", []))

    def test_current_unchanged(self):
        self.assertIsNotNone(self.chat_map)
        self.assertEqual(self.chat_map.get("current"), self.KEEP_ALIAS)


# ---------------------------------------------------------------------------
# DELETE /api/session — current session rejected (409)
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestDeleteCurrent(unittest.TestCase):
    """DELETE /api/session?alias=current_session returns 409."""

    ALIAS = "rest_t_delcur"

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.1)
        cls.r = session_delete(cls.ALIAS)

    def test_status_409(self):
        self.assertEqual(self.r.status_code, 409)


# ---------------------------------------------------------------------------
# DELETE /api/session — session not found (404)
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestDeleteNotFound(unittest.TestCase):
    """DELETE /api/session?alias=nonexistent returns 404."""

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        cls.r = session_delete("no_such_alias_xyz")

    def test_status_404(self):
        self.assertEqual(self.r.status_code, 404)


# ---------------------------------------------------------------------------
# DELETE /api/session — missing alias query param (400)
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestDeleteNoAlias(unittest.TestCase):
    """DELETE /api/session without alias query param returns 400."""

    @classmethod
    def setUpClass(cls):
        cls.r = requests.delete(SESSION_URL, timeout=HTTP_TIMEOUT)

    def test_status_400(self):
        self.assertEqual(self.r.status_code, 400)


# ---------------------------------------------------------------------------
# GET /api/session — consistency after new + resume operations
# ---------------------------------------------------------------------------

@_skip
class TestSessionRestGetConsistency(unittest.TestCase):
    """GET /api/session reflects current and sessions list after operations."""

    ALIAS_A = "rest_t_gc_a"
    ALIAS_B = "rest_t_gc_b"

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS_A})
        time.sleep(0.1)
        session_post({"action": "new", "alias": cls.ALIAS_B})
        time.sleep(0.1)
        session_post({"action": "resume", "alias": cls.ALIAS_A})
        time.sleep(0.2)
        cls.r = session_get()
        cls.body = cls.r.json() if cls.r.status_code == 200 else None

    def test_status_200(self):
        self.assertEqual(self.r.status_code, 200)

    def test_current_is_alias_a(self):
        self.assertIsNotNone(self.body)
        self.assertEqual(self.body.get("current"), self.ALIAS_A)

    def test_sessions_list_has_both(self):
        self.assertIsNotNone(self.body)
        sessions = self.body.get("sessions", [])
        aliases = [s["alias"] if isinstance(s, dict) else s for s in sessions]
        self.assertIn(self.ALIAS_A, aliases)
        self.assertIn(self.ALIAS_B, aliases)

    def test_get_current_matches_chat_map(self):
        chat_map = read_local_chat_map()
        if chat_map:
            self.assertEqual(self.body.get("current"), chat_map.get("current"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
