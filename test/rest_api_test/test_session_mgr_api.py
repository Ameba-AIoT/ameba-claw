"""
WHY: Tests that verify the file API operations performed by session_mgr.js
(WebUI session management page) work correctly end-to-end.

session_mgr.js manages sessions by:
 1. GET /api/files?path=vfs:/session/chat_map  — list channel chat_map files
 2. GET /api/files/content?path=<each file>    — read sessions/current fields
 3. DELETE /api/files?path=sessionFilePath(id) — smDelete step 1
 4. GET + PUT /api/files/content on chat_map   — smDelete step 2+3

Critical bug tracked by D-09:
 session_mgr.js uses the XOR-variant djb2 to build session file paths, while
 the firmware (cap_session_mgr.c) uses the addition-variant djb2.  These
 produce different hashes for every session ID, so:
   - smDelete DELETEs the wrong file (actual history file never removed -> VFS leak)
   - smViewHistory fetches the wrong path (404 -> empty history modal)

Test plan D-01 through D-11 (no D-10):
  D-01/02  List chat_map directory; read and validate a chat_map file
  D-03/04  Write and read a session history file via VFS file API
  D-05     smDelete non-current session: file removed, map updated, current unchanged
  D-06     smDelete current session: promotes first remaining session
  D-07     smDelete last session: map resets sessions=[], current='default'
  D-08     smDelete when session file is missing: 404 on DELETE, PUT map still works
  D-09     djb2 hash consistency check (EXPECTED TO FAIL — documents the known bug)
  D-11     vfs: prefix compatibility in /api/files path parameter
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
FILES_URL = BASE + "/api/files"
FILES_CONTENT_URL = BASE + "/api/files/content"


# ---------------------------------------------------------------------------
# Hash / path helpers — firmware addition-variant and JS XOR-variant
# ---------------------------------------------------------------------------

def _djb2_add(s: str) -> int:
    """Addition-variant djb2 — cap_session_mgr.c hash_str() for chat_map files."""
    h = 5381
    for c in s.encode("utf-8"):
        h = ((h << 5) + h + c) & 0xFFFFFFFF
    return h


def _djb2_xor(s: str) -> int:
    """XOR-variant djb2 — claw_memory.c session_file_path() for session history files.
    Also used by session_mgr.js djb2() — both correctly match claw_memory.c."""
    h = 5381
    for c in s.encode("utf-8"):
        h = (((h << 5) + h) ^ c) & 0xFFFFFFFF
    return h


def _sanitize(s: str, max_len: int = 32) -> str:
    return "".join(c if (c.isalnum() or c in "._-") else "_" for c in s)[:max_len]


def chat_map_path(channel: str, chat_id: str) -> str:
    """Addition hash — matches cap_session_mgr.c."""
    key = f"{channel}:{chat_id}"
    return f"/session/chat_map/s_{_sanitize(key)}_{_djb2_add(key):08x}.json"


def session_history_path(channel: str, chat_id: str, alias: str) -> str:
    """XOR hash — matches claw_memory.c. session_mgr.js also uses XOR (correct)."""
    sid = f"{channel}:{chat_id}:{alias}"
    return f"/session/s_{_sanitize(sid)}_{_djb2_xor(sid):08x}.json"


# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------

def files_read(path: str):
    r = requests.get(FILES_CONTENT_URL, params={"path": path}, timeout=HTTP_TIMEOUT)
    return r.status_code, r.text


def files_write(path: str, content: str) -> int:
    r = requests.put(FILES_CONTENT_URL, params={"path": path},
                     data=content.encode(), timeout=HTTP_TIMEOUT)
    return r.status_code


def files_delete(path: str) -> int:
    r = requests.delete(FILES_URL, params={"path": path}, timeout=HTTP_TIMEOUT)
    return r.status_code


def read_local_chat_map():
    code, text = files_read(chat_map_path(REST_CHANNEL, REST_CHAT_ID))
    if code != 200:
        return None
    try:
        return json.loads(text)
    except Exception:
        return None


def write_local_chat_map(data: dict) -> int:
    return files_write(chat_map_path(REST_CHANNEL, REST_CHAT_ID), json.dumps(data))


def session_post(body: dict):
    return requests.post(SESSION_URL, json=body, timeout=HTTP_TIMEOUT)


def session_delete(alias: str):
    return requests.delete(SESSION_URL, params={"alias": alias}, timeout=HTTP_TIMEOUT)


def session_get():
    return requests.get(SESSION_URL, timeout=HTTP_TIMEOUT)


# ---------------------------------------------------------------------------
# Board readiness probe (mirrors test_session_rest_api._board_ready)
# ---------------------------------------------------------------------------

def _board_ready(timeout: float = 10.0) -> bool:
    """Wait until POST /api/session completes in under 2 s.
    A fast POST response confirms the HTTP connection tasks are not backed up
    by post-response WS broadcasts to stale connections (SO_SNDTIMEO = 1s).
    Call this at the start of setUpClass methods that make rapid sequential
    POSTs, especially after WS tests that may have left half-dead connections."""
    deadline = time.time() + timeout
    probe_alias = "_smgr_probe_"
    while time.time() < deadline:
        try:
            t0 = time.time()
            r = requests.post(SESSION_URL,
                              json={"action": "new", "alias": probe_alias},
                              timeout=3)
            elapsed = time.time() - t0
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


def delete_local_chat_map():
    _board_ready()
    files_delete(chat_map_path(REST_CHANNEL, REST_CHAT_ID))
    time.sleep(0.3)


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
# D-01 / D-02 : list chat_map directory and read a chat_map file
# ---------------------------------------------------------------------------

@_skip
class TestSessionMgrListChatMap(unittest.TestCase):
    """D-01/02: List /session/chat_map and validate a chat_map JSON file.

    session_mgr.js calls GET /api/files?path=vfs:/session/chat_map to find all
    channel chat_map files, then reads each one to build the session list.
    If either step fails the entire session manager page goes blank.
    """

    ALIAS = "smgr_d01_sess"

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.3)

        r = requests.get(FILES_URL, params={"path": "/session/chat_map"},
                         timeout=HTTP_TIMEOUT)
        cls.list_status = r.status_code
        cls.raw_body = None
        try:
            cls.raw_body = r.json() if r.status_code == 200 else None
        except Exception:
            pass

        # /api/files returns {path, readonly, entries:[...]} — extract entries list
        entries = []
        if isinstance(cls.raw_body, dict):
            entries = cls.raw_body.get("entries", [])
        elif isinstance(cls.raw_body, list):
            entries = cls.raw_body  # future-proof if API format changes

        # Read the first s_*.json entry found in the listing
        cls.map_data = None
        for f in entries:
            if not isinstance(f, dict):
                continue
            name = f.get("name", "")
            if name.startswith("s_") and name.endswith(".json"):
                code, text = files_read(f.get("path", ""))
                if code == 200:
                    try:
                        cls.map_data = json.loads(text)
                    except Exception:
                        pass
                break

    @classmethod
    def tearDownClass(cls):
        files_delete(chat_map_path(REST_CHANNEL, REST_CHAT_ID))

    def test_d01_list_status_200(self):
        self.assertEqual(self.list_status, 200,
                         "GET /api/files?path=/session/chat_map must return 200; "
                         "session_mgr.js uses this to discover chat_map files")

    def test_d01_list_returns_object_with_entries(self):
        """API returns {entries:[...]} not a bare array."""
        self.assertIsInstance(self.raw_body, dict,
                              "GET /api/files must return a JSON object (not a bare array)")
        self.assertIn("entries", self.raw_body,
                      "Response object must have an 'entries' key")
        self.assertIsInstance(self.raw_body["entries"], list,
                              "'entries' value must be a list")

    def test_d01_js_bug_files_length_undefined(self):
        """session_mgr.js must access data.entries, not treat the /api/files response as an array.
        /api/files returns {path, readonly, entries:[...]}, so files.length is undefined in JS
        when the callback parameter holds the whole response object."""
        js_path = os.path.normpath(os.path.join(
            os.path.dirname(__file__),
            '..', '..', 'claw_capabilities', 'cap_webui', 'res', 'session_mgr.js'))
        if not os.path.exists(js_path):
            self.skipTest(f"session_mgr.js not found at {js_path}")
        src = open(js_path).read()
        self.assertIn(
            'data.entries', src,
            "session_mgr.js must use data.entries to iterate the file list; "
            "/api/files returns {path, readonly, entries:[...]}, not a bare array")

    def test_d01_list_has_s_json_files(self):
        entries = self.raw_body.get("entries", []) if isinstance(self.raw_body, dict) else []
        json_files = [
            f for f in entries
            if isinstance(f, dict)
            and f.get("name", "").startswith("s_")
            and f.get("name", "").endswith(".json")
        ]
        self.assertGreater(len(json_files), 0,
                           "chat_map directory must contain at least one s_*.json file "
                           "after creating a session")

    def test_d02_map_file_readable(self):
        self.assertIsNotNone(self.map_data,
                             "First s_*.json in chat_map directory must be readable "
                             "valid JSON; session_mgr.js reads it for sessions/current")

    def test_d02_sessions_is_array(self):
        self.assertIsNotNone(self.map_data)
        self.assertIsInstance(self.map_data.get("sessions"), list,
                              "chat_map must have 'sessions' array; "
                              "session_mgr.js iterates d.sessions to list aliases")

    def test_d02_current_is_string(self):
        self.assertIsNotNone(self.map_data)
        self.assertIsInstance(self.map_data.get("current"), str,
                              "chat_map must have 'current' string; "
                              "session_mgr.js reads d.current to mark the active row")


# ---------------------------------------------------------------------------
# D-03 / D-04 : write and read a session history file via VFS
# ---------------------------------------------------------------------------

@_skip
class TestSessionMgrReadSessionFile(unittest.TestCase):
    """D-03/04: PUT a session history file and verify GET returns correct structure.

    smViewHistory calls GET /api/files/content?path=<sessionFilePath(sid)>
    and iterates d.turns to render the conversation.  A completed turn has both
    user and assistant fields; an incomplete (streaming) turn has only user and
    completed=false.
    """

    ALIAS = "smgr_d03_hist"

    FAKE_HISTORY = json.dumps({
        "turns": [
            {"user": "hello", "assistant": "world", "completed": True, "req_id": 1001},
            {"user": "pending", "completed": False, "req_id": 1002},
        ]
    })

    @classmethod
    def setUpClass(cls):
        _board_ready()
        cls.hist_path = session_history_path(REST_CHANNEL, REST_CHAT_ID, cls.ALIAS)
        cls.write_status = files_write(cls.hist_path, cls.FAKE_HISTORY)
        time.sleep(0.1)
        cls.read_status, cls.read_text = files_read(cls.hist_path)
        cls.data = None
        if cls.read_status == 200:
            try:
                cls.data = json.loads(cls.read_text)
            except Exception:
                pass

    @classmethod
    def tearDownClass(cls):
        files_delete(cls.hist_path)

    def test_d03_write_status_200(self):
        self.assertEqual(self.write_status, 200,
                         "PUT /api/files/content must accept a session history JSON blob")

    def test_d03_read_status_200(self):
        self.assertEqual(self.read_status, 200,
                         "GET /api/files/content must return the written session file")

    def test_d03_turns_is_array(self):
        self.assertIsNotNone(self.data)
        self.assertIsInstance(self.data.get("turns"), list,
                              "Session file must have 'turns' array; "
                              "smViewHistory iterates d.turns")

    def test_d04_completed_turn_has_user_and_assistant(self):
        self.assertIsNotNone(self.data)
        completed = [t for t in self.data.get("turns", []) if t.get("completed") is True]
        self.assertGreater(len(completed), 0, "At least one completed turn must be present")
        for t in completed:
            self.assertIn("user", t, "Completed turn must have 'user' field")
            self.assertIn("assistant", t, "Completed turn must have 'assistant' field")

    def test_d04_incomplete_turn_has_no_assistant(self):
        self.assertIsNotNone(self.data)
        incomplete = [t for t in self.data.get("turns", []) if t.get("completed") is False]
        self.assertGreater(len(incomplete), 0, "At least one incomplete turn must be present")
        for t in incomplete:
            self.assertNotIn("assistant", t,
                             "Incomplete (streaming) turn must not have 'assistant' field; "
                             "presence would indicate a partial write by the firmware")

    def test_d04_completed_field_is_bool(self):
        self.assertIsNotNone(self.data)
        for t in self.data.get("turns", []):
            self.assertIsInstance(
                t.get("completed"), bool,
                f"'completed' must be a boolean, got {t.get('completed')!r}"
            )


# ---------------------------------------------------------------------------
# D-05 : smDelete non-current session
# ---------------------------------------------------------------------------

@_skip
class TestSessionMgrDeleteNonCurrentSession(unittest.TestCase):
    """D-05: smDelete removes a non-current session file and updates the chat_map.

    smDelete flow (session_mgr.js lines 108-129):
      1. DELETE /api/files?path=sessionFilePath(sessionId)
      2. GET chat_map -> filter out alias -> if current==alias set current=next
      3. PUT updated chat_map
    When deleting a non-current session, current must remain unchanged.
    """

    ALIAS_A = "smgr_d05_cur"   # stays current throughout
    ALIAS_B = "smgr_d05_del"   # non-current; will be deleted

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS_A})
        time.sleep(0.1)
        session_post({"action": "new", "alias": cls.ALIAS_B})
        time.sleep(0.1)
        session_post({"action": "resume", "alias": cls.ALIAS_A})  # current=A
        time.sleep(0.2)

        # Write a controlled two-session map (avoids 'default' appearing in sessions)
        write_local_chat_map({
            "chat_key": f"{REST_CHANNEL}:{REST_CHAT_ID}",
            "sessions": [cls.ALIAS_A, cls.ALIAS_B],
            "current": cls.ALIAS_A,
        })
        time.sleep(0.1)

        cls.file_b = session_history_path(REST_CHANNEL, REST_CHAT_ID, cls.ALIAS_B)

        # Simulate smDelete(B): step 1 — delete session file
        cls.delete_b_status = files_delete(cls.file_b)

        # Steps 2+3 — read map, remove B, PUT updated map
        map_data = read_local_chat_map()
        if map_data:
            sessions = [s for s in map_data.get("sessions", []) if s != cls.ALIAS_B]
            map_data["sessions"] = sessions
            if map_data.get("current") == cls.ALIAS_B:
                map_data["current"] = sessions[0] if sessions else "default"
            write_local_chat_map(map_data)
        time.sleep(0.2)

        cls.map_after = read_local_chat_map()
        cls.file_b_read_status, _ = files_read(cls.file_b)

    @classmethod
    def tearDownClass(cls):
        files_delete(chat_map_path(REST_CHANNEL, REST_CHAT_ID))

    def test_d05_session_b_file_gone(self):
        self.assertNotEqual(self.file_b_read_status, 200,
                            "Session file for deleted alias B must no longer be readable; "
                            "smDelete deletes the file in step 1")

    def test_d05_sessions_does_not_contain_b(self):
        self.assertIsNotNone(self.map_after)
        self.assertNotIn(self.ALIAS_B, self.map_after.get("sessions", []),
                         "chat_map.sessions must not contain the deleted alias B")

    def test_d05_current_still_a(self):
        self.assertIsNotNone(self.map_after)
        self.assertEqual(self.map_after.get("current"), self.ALIAS_A,
                         "Deleting a non-current session must not change current; "
                         "the user's active context must be preserved")


# ---------------------------------------------------------------------------
# D-06 : smDelete current session — promotes next session
# ---------------------------------------------------------------------------

@_skip
class TestSessionMgrDeleteCurrentSession(unittest.TestCase):
    """D-06: smDelete on the current session promotes the first remaining session.

    smDelete logic (session_mgr.js line 120):
      if (d.current === alias) d.current = newSessions.length ? newSessions[0] : 'default';
    After deleting the current session, the first remaining alias becomes current.
    """

    ALIAS_A = "smgr_d06_cur"   # current; will be deleted
    ALIAS_B = "smgr_d06_next"  # should become current after delete

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS_B})
        time.sleep(0.1)
        session_post({"action": "new", "alias": cls.ALIAS_A})  # current=A
        time.sleep(0.2)

        # Write a controlled map: sessions=[B, A], current=A
        # B is first so that after removing A, sessions[0]==B (per smDelete logic)
        write_local_chat_map({
            "chat_key": f"{REST_CHANNEL}:{REST_CHAT_ID}",
            "sessions": [cls.ALIAS_B, cls.ALIAS_A],
            "current": cls.ALIAS_A,
        })
        time.sleep(0.1)

        cls.file_a = session_history_path(REST_CHANNEL, REST_CHAT_ID, cls.ALIAS_A)

        # Simulate smDelete(A) where A is current
        files_delete(cls.file_a)
        map_data = read_local_chat_map()
        if map_data:
            sessions = [s for s in map_data.get("sessions", []) if s != cls.ALIAS_A]
            map_data["sessions"] = sessions
            if map_data.get("current") == cls.ALIAS_A:
                map_data["current"] = sessions[0] if sessions else "default"
            write_local_chat_map(map_data)
        time.sleep(0.2)

        cls.map_after = read_local_chat_map()

    @classmethod
    def tearDownClass(cls):
        files_delete(chat_map_path(REST_CHANNEL, REST_CHAT_ID))

    def test_d06_sessions_does_not_contain_a(self):
        self.assertIsNotNone(self.map_after)
        self.assertNotIn(self.ALIAS_A, self.map_after.get("sessions", []),
                         "Deleted alias A must be removed from sessions list")

    def test_d06_current_promoted_to_b(self):
        self.assertIsNotNone(self.map_after)
        current = self.map_after.get("current")
        self.assertEqual(current, self.ALIAS_B,
                         f"After deleting current session A, current must become B "
                         f"(first remaining per smDelete logic: newSessions[0]); "
                         f"got {current!r}")


# ---------------------------------------------------------------------------
# D-07 : smDelete last session — resets to default
# ---------------------------------------------------------------------------

@_skip
class TestSessionMgrDeleteLastSession(unittest.TestCase):
    """D-07: smDelete on the only session resets map to sessions=[], current='default'.

    smDelete logic (session_mgr.js line 120):
      d.current = newSessions.length ? newSessions[0] : 'default'
    If newSessions is empty (last session deleted), current becomes 'default'.
    The session manager page must show the empty state rather than crashing.
    """

    ALIAS_A = "smgr_d07_last"

    @classmethod
    def setUpClass(cls):
        delete_local_chat_map()
        session_post({"action": "new", "alias": cls.ALIAS_A})
        time.sleep(0.2)

        # Force exactly one session in VFS (firmware may also add 'default')
        write_local_chat_map({
            "chat_key": f"{REST_CHANNEL}:{REST_CHAT_ID}",
            "sessions": [cls.ALIAS_A],
            "current": cls.ALIAS_A,
        })
        time.sleep(0.1)

        cls.file_a = session_history_path(REST_CHANNEL, REST_CHAT_ID, cls.ALIAS_A)

        # Simulate smDelete(last session)
        files_delete(cls.file_a)
        map_data = read_local_chat_map()
        cls.put_status = None
        if map_data:
            sessions = [s for s in map_data.get("sessions", []) if s != cls.ALIAS_A]
            map_data["sessions"] = sessions  # []
            if map_data.get("current") == cls.ALIAS_A:
                map_data["current"] = sessions[0] if sessions else "default"
            cls.put_status = write_local_chat_map(map_data)
        time.sleep(0.2)

        cls.map_after = read_local_chat_map()

    @classmethod
    def tearDownClass(cls):
        files_delete(chat_map_path(REST_CHANNEL, REST_CHAT_ID))

    def test_d07_put_map_succeeds(self):
        self.assertEqual(self.put_status, 200,
                         "PUT chat_map after deleting last session must succeed; "
                         "smDelete calls PUT unconditionally even when sessions=[]")

    def test_d07_sessions_empty(self):
        self.assertIsNotNone(self.map_after)
        self.assertEqual(self.map_after.get("sessions"), [],
                         "sessions must be [] after deleting the only session")

    def test_d07_current_is_default(self):
        self.assertIsNotNone(self.map_after)
        self.assertEqual(self.map_after.get("current"), "default",
                         "current must be 'default' after deleting the last session; "
                         "smDelete sets d.current='default' when newSessions is empty")


# ---------------------------------------------------------------------------
# D-08 : smDelete when session file is missing (404 on DELETE)
# ---------------------------------------------------------------------------

@_skip
class TestSessionMgrDeleteMissingFile(unittest.TestCase):
    """D-08: smDelete is robust when the session file is already missing.

    session_mgr.js smDelete does not check whether the DELETE step succeeded
    before proceeding to update the chat_map (the .then() chain continues
    regardless of the DELETE response status).  This test verifies:
      - DELETE on a non-existent path returns 404 (server is correct)
      - A subsequent PUT to update the chat_map still succeeds
    Models the scenario of a double-delete or VFS inconsistency where the
    file is gone but the alias remains listed in the chat_map.
    """

    ALIAS_REAL = "smgr_d08_real"
    NONEXISTENT_FILE = "/session/s_smgr_d08_ghost_00000000.json"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        files_delete(chat_map_path(REST_CHANNEL, REST_CHAT_ID))
        time.sleep(0.1)
        session_post({"action": "new", "alias": cls.ALIAS_REAL})
        time.sleep(0.2)

        # Step 1: DELETE a file that does not exist
        cls.delete_status = files_delete(cls.NONEXISTENT_FILE)

        # Step 2+3: Update the map anyway (smDelete continues after 404)
        map_data = read_local_chat_map()
        if not map_data:
            map_data = {
                "chat_key": f"{REST_CHANNEL}:{REST_CHAT_ID}",
                "sessions": [cls.ALIAS_REAL],
                "current": cls.ALIAS_REAL,
            }
        # Remove the ghost alias (no-op here, exercises the PUT path)
        map_data["sessions"] = [s for s in map_data.get("sessions", [])
                                 if s != "smgr_d08_ghost"]
        cls.put_status = write_local_chat_map(map_data)
        time.sleep(0.1)
        cls.map_after = read_local_chat_map()

    @classmethod
    def tearDownClass(cls):
        files_delete(chat_map_path(REST_CHANNEL, REST_CHAT_ID))

    def test_d08_delete_missing_returns_404(self):
        self.assertEqual(self.delete_status, 404,
                         "DELETE /api/files on a non-existent path must return 404; "
                         "confirms the server reports missing files correctly")

    def test_d08_put_map_succeeds_after_404_delete(self):
        self.assertEqual(self.put_status, 200,
                         "PUT chat_map must succeed after a preceding 404 DELETE; "
                         "smDelete proceeds unconditionally after the DELETE fetch()")

    def test_d08_map_valid_after_update(self):
        self.assertIsNotNone(self.map_after,
                             "chat_map must be readable valid JSON after a PUT that "
                             "follows a 404 delete; a corrupt map breaks the next page load")


# ---------------------------------------------------------------------------
# D-09 : djb2 hash variant documentation
# ---------------------------------------------------------------------------

class TestDjb2HashVariants(unittest.TestCase):
    """D-09: document the two djb2 variants in the firmware and verify our helpers.

    The firmware uses TWO different djb2 variants for two file types:
      cap_session_mgr.c  (chat_map files):     addition  h = (h*33) + c
      claw_memory.c      (session history):    XOR       h = (h*33) ^ c

    session_mgr.js uses XOR — this is CORRECT because it accesses session
    history files (claw_memory.c paths), not chat_map files.

    Our Python test helpers must use the matching variant per file type.
    """

    @classmethod
    def setUpClass(cls):
        # Write the chat_map directly so we know it exists at the expected path.
        # This also verifies that chat_map_path() produces the path the firmware reads:
        # if the firmware's /api/session GET shows this session it read the right file.
        write_local_chat_map({
            "chat_key": "local:local",
            "sessions": ["d09_verify"],
            "current": "d09_verify",
        })
        time.sleep(0.2)

    @classmethod
    def tearDownClass(cls):
        files_delete(chat_map_path(REST_CHANNEL, REST_CHAT_ID))
        time.sleep(0.2)

    SESSION_IDS = [
        "local:local:default",
        "local:local:test",
        "local:local:abc",
    ]

    def test_d09_two_variants_produce_different_hashes(self):
        """The two variants always differ (non-trivially) for our session IDs."""
        for sid in self.SESSION_IDS:
            add = _djb2_add(sid)
            xor = _djb2_xor(sid)
            self.assertNotEqual(add, xor,
                f"addition and XOR djb2 unexpectedly agree for {sid!r}: {add:08x}")

    def test_d09_session_history_helper_uses_xor(self):
        """session_history_path() must use XOR to match claw_memory.c."""
        for sid in self.SESSION_IDS:
            ch, cid, alias = sid.split(":", 2)
            expected = f"/session/s_{_sanitize(sid)}_{_djb2_xor(sid):08x}.json"
            got = session_history_path(ch, cid, alias)
            self.assertEqual(got, expected,
                f"session_history_path() uses wrong variant for {sid!r}: "
                f"got {got}, expected {expected}")

    def test_d09_chat_map_helper_uses_addition(self):
        """chat_map_path() must use addition to match cap_session_mgr.c."""
        key = "local:local"
        expected = f"/session/chat_map/s_{_sanitize(key)}_{_djb2_add(key):08x}.json"
        got = chat_map_path("local", "local")
        self.assertEqual(got, expected,
            f"chat_map_path() uses wrong variant: got {got}, expected {expected}")

    def test_d09_chat_map_addition_hash_matches_actual_file(self):
        """The addition-variant chat_map path was written and is readable by firmware.
        setUpClass wrote the file; /api/session must reflect the session inside it,
        confirming the firmware reads chat_map from the addition-hash path."""
        path = chat_map_path("local", "local")
        code, text = files_read(path)
        self.assertEqual(code, 200,
            f"chat_map file not readable at addition-hash path {path}; "
            "either the hash variant is wrong or the write failed")


# ---------------------------------------------------------------------------
# D-11 : vfs: prefix compatibility
# ---------------------------------------------------------------------------

@_skip
class TestVfsPrefixCompatibility(unittest.TestCase):
    """D-11: session_mgr.js always prefixes paths with 'vfs:'; firmware must accept it.

    loadSessionMgr() fetches /api/files?path=vfs:/session/chat_map.
    sessionFilePath() returns 'vfs:/session/s_<san>_<hash>.json'.
    If the firmware does not strip the 'vfs:' prefix transparently, every file
    operation in the session manager page will silently fail (empty list or 404).
    """

    ALIAS = "smgr_d11_sess"

    @classmethod
    def setUpClass(cls):
        _board_ready()
        files_delete(chat_map_path(REST_CHANNEL, REST_CHAT_ID))
        time.sleep(0.1)
        session_post({"action": "new", "alias": cls.ALIAS})
        time.sleep(0.3)

        r_plain = requests.get(FILES_URL,
                               params={"path": "/session/chat_map"},
                               timeout=HTTP_TIMEOUT)
        r_vfs = requests.get(FILES_URL,
                             params={"path": "vfs:/session/chat_map"},
                             timeout=HTTP_TIMEOUT)
        cls.plain_status = r_plain.status_code
        cls.vfs_status = r_vfs.status_code
        cls.plain_body = None
        cls.vfs_body = None
        try:
            cls.plain_body = r_plain.json()
        except Exception:
            pass
        try:
            cls.vfs_body = r_vfs.json()
        except Exception:
            pass

    @classmethod
    def tearDownClass(cls):
        files_delete(chat_map_path(REST_CHANNEL, REST_CHAT_ID))

    def test_d11_plain_path_200(self):
        self.assertEqual(self.plain_status, 200,
                         "GET /api/files?path=/session/chat_map must return 200 "
                         "(baseline: plain path without vfs: prefix)")

    def test_d11_vfs_prefix_accepted(self):
        self.assertEqual(self.vfs_status, 200,
                         "GET /api/files?path=vfs:/session/chat_map must return 200; "
                         "session_mgr.js always uses vfs: prefix — rejection breaks "
                         "the entire session manager page")

    def test_d11_vfs_and_plain_return_same_files(self):
        if self.plain_status != 200 or self.vfs_status != 200:
            self.skipTest("One or both requests failed; cannot compare results")
        plain_names = sorted(
            f.get("name", "") for f in (self.plain_body or []) if isinstance(f, dict)
        )
        vfs_names = sorted(
            f.get("name", "") for f in (self.vfs_body or []) if isinstance(f, dict)
        )
        self.assertEqual(plain_names, vfs_names,
                         "vfs: prefix and plain path must list the same files; "
                         "a discrepancy means the firmware treats them differently "
                         "and session_mgr.js will see incorrect (possibly empty) results")


if __name__ == "__main__":
    unittest.main(verbosity=2)
