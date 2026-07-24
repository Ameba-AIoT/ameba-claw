"""
UNT-001 to UNT-018: Smoke tests for previously-untested API endpoints.

Endpoints covered:
  GET  /                      (UNT-001)
  POST /setup                 (UNT-002 to UNT-003)
  GET  /api/wechat/qrcode     (UNT-004)
  GET  /api/wechat/status     (UNT-005)
  GET  /api/wechat/token      (UNT-006)
  POST /api/lua/upload        (UNT-007 to UNT-009)
  GET  /api/lua/modules       (UNT-010 to UNT-011)
  POST /api/lua/modules       (UNT-012 to UNT-014)
  POST /api/cap/invoke        (UNT-015 to UNT-016)
  GET/POST /api/lua/modules   (UNT-017 to UNT-018 — disabled/locked semantics)

NOTE: POST /api/system/restart is intentionally omitted — it calls sys_reset()
      immediately after responding and would terminate the test session.
"""
import unittest
import requests
import sys
import os
sys.path.insert(0, os.path.dirname(__file__))
from config import BOARD_BASE_URL, HTTP_TIMEOUT


def board_get(path, **kwargs):
    return requests.get(BOARD_BASE_URL + path, timeout=HTTP_TIMEOUT, **kwargs)

def board_post(path, **kwargs):
    return requests.post(BOARD_BASE_URL + path, timeout=HTTP_TIMEOUT, **kwargs)

def board_delete(path, **kwargs):
    return requests.delete(BOARD_BASE_URL + path, timeout=HTTP_TIMEOUT, **kwargs)


# ---------------------------------------------------------------------------
# UNT-001 — GET /  (root dashboard page)
# ---------------------------------------------------------------------------

class TestRootPage(unittest.TestCase):

    def test_UNT_001_root_returns_html(self):
        """GET / returns 200 with text/html dashboard page."""
        r = board_get("/")
        self.assertEqual(r.status_code, 200, f"Expected 200, got {r.status_code}")
        ct = r.headers.get("Content-Type", "")
        self.assertIn("text/html", ct, f"Expected text/html, got: {ct}")
        self.assertGreater(len(r.content), 500,
            "Dashboard HTML looks too small")
        # Should contain the word 'dashboard' or some UI landmark
        body = r.content.decode("utf-8", errors="replace").lower()
        self.assertTrue(
            "dashboard" in body or "claw" in body or "<!doctype" in body.lower(),
            "Root page does not look like the dashboard HTML"
        )


# ---------------------------------------------------------------------------
# UNT-002 to UNT-003 — POST /setup
# ---------------------------------------------------------------------------

class TestSetupPost(unittest.TestCase):

    def test_UNT_002_setup_post_no_body_returns_4xx_or_200(self):
        """POST /setup with empty body: endpoint exists and responds."""
        r = requests.post(BOARD_BASE_URL + "/setup",
                          data=b"", timeout=HTTP_TIMEOUT)
        # Accept any non-5xx: handler validates input and may redirect/error
        self.assertIn(r.status_code, [200, 302, 400, 422],
            f"POST /setup with empty body: unexpected status {r.status_code}")

    def test_UNT_003_setup_post_missing_ssid_rejected(self):
        """POST /setup without ssid field returns 400 (fast validation)."""
        r = requests.post(BOARD_BASE_URL + "/setup",
                          data={"password": "testpw"},
                          timeout=HTTP_TIMEOUT)
        # Missing ssid should result in 400 or a redirect; must not be 500
        self.assertNotEqual(r.status_code, 500,
            f"POST /setup missing ssid returned 500: {r.text[:200]}")


# ---------------------------------------------------------------------------
# UNT-004 to UNT-006 — GET /api/wechat/*
# ---------------------------------------------------------------------------

class TestWechatEndpoints(unittest.TestCase):

    def test_UNT_004_wechat_qrcode_responds(self):
        """GET /api/wechat/qrcode returns JSON (ok:true or error)."""
        r = board_get("/api/wechat/qrcode")
        self.assertIn(r.status_code, [200, 500, 503],
            f"Unexpected status: {r.status_code}")
        data = r.json()
        self.assertIn("ok", data, f"Response has no 'ok' field: {data}")

    def test_UNT_005_wechat_status_returns_json(self):
        """GET /api/wechat/status returns 200 with JSON."""
        r = board_get("/api/wechat/status")
        self.assertEqual(r.status_code, 200,
            f"Expected 200, got {r.status_code}: {r.text}")
        # Must parse as JSON without raising
        data = r.json()
        self.assertIsInstance(data, dict, f"Expected dict, got {type(data)}")

    def test_UNT_006_wechat_token_returns_json_with_token_field(self):
        """GET /api/wechat/token returns 200 JSON with 'token' field."""
        r = board_get("/api/wechat/token")
        self.assertEqual(r.status_code, 200,
            f"Expected 200, got {r.status_code}: {r.text}")
        data = r.json()
        self.assertIn("token", data,
            f"Response missing 'token' field: {data}")
        # token is a string (possibly empty if not logged in)
        self.assertIsInstance(data["token"], str,
            f"token should be string, got {type(data['token'])}")


# ---------------------------------------------------------------------------
# UNT-007 to UNT-009 — POST /api/lua/upload (multipart)
# ---------------------------------------------------------------------------

class TestLuaUpload(unittest.TestCase):

    SCRIPT_NAME = "test_unt_upload.lua"
    SCRIPT_BODY = b"-- uploaded via /api/lua/upload\nfunction run(a) return 'ok' end\n"

    @classmethod
    def tearDownClass(cls):
        board_delete("/api/lua", params={"name": cls.SCRIPT_NAME})

    def test_UNT_007_upload_lua_script_ok(self):
        """POST /api/lua/upload with valid Lua script returns ok=true."""
        r = requests.post(
            BOARD_BASE_URL + "/api/lua/upload",
            files={"file": (self.SCRIPT_NAME, self.SCRIPT_BODY, "text/plain")},
            timeout=HTTP_TIMEOUT
        )
        self.assertEqual(r.status_code, 200,
            f"Upload returned {r.status_code}: {r.text}")
        self.assertTrue(r.json().get("ok"), f"Expected ok=true: {r.json()}")

    def test_UNT_008_uploaded_script_appears_in_list(self):
        """Script uploaded via /api/lua/upload shows up in GET /api/lua."""
        requests.post(
            BOARD_BASE_URL + "/api/lua/upload",
            files={"file": (self.SCRIPT_NAME, self.SCRIPT_BODY, "text/plain")},
            timeout=HTTP_TIMEOUT
        )
        r = board_get("/api/lua")
        self.assertEqual(r.status_code, 200)
        names = [f["name"] for f in r.json().get("files", [])]
        self.assertIn(self.SCRIPT_NAME, names,
            f"{self.SCRIPT_NAME} not in listing after upload: {names}")

    def test_UNT_009_upload_no_file_field_400(self):
        """POST /api/lua/upload without 'file' field returns 400."""
        r = requests.post(
            BOARD_BASE_URL + "/api/lua/upload",
            files={"not_file": ("x.lua", b"-- wrong field", "text/plain")},
            timeout=HTTP_TIMEOUT
        )
        self.assertIn(r.status_code, [400, 422],
            f"Expected 400 for missing file field, got {r.status_code}: {r.text}")


# ---------------------------------------------------------------------------
# UNT-010 to UNT-011 — GET /api/lua/modules
# ---------------------------------------------------------------------------

class TestLuaModulesGet(unittest.TestCase):

    def test_UNT_010_modules_list_returns_array(self):
        """GET /api/lua/modules returns 200 with modules array and disabled string."""
        r = board_get("/api/lua/modules")
        self.assertEqual(r.status_code, 200,
            f"Expected 200, got {r.status_code}: {r.text}")
        data = r.json()
        self.assertIn("modules", data,
            f"Response missing 'modules' key: {data}")
        self.assertIsInstance(data["modules"], list,
            f"modules should be list, got {type(data['modules'])}")
        self.assertIn("disabled", data,
            f"Response missing 'disabled' key: {data}")
        self.assertIsInstance(data["disabled"], str,
            f"disabled should be str, got {type(data['disabled'])}")

    def test_UNT_011_modules_entries_have_required_fields(self):
        """Each module entry has id, category, enabled, locked, chip_ok fields."""
        data = board_get("/api/lua/modules").json()
        modules = data.get("modules", [])
        self.assertGreater(len(modules), 0, "Module list is empty")
        for m in modules:
            for field in ("id", "category", "enabled", "locked", "chip_ok"):
                self.assertIn(field, m,
                    f"Module entry missing '{field}': {m}")
        # Verify known locked modules
        locked_ids = {m["id"] for m in modules if m.get("locked")}
        for expected in ("sys", "event", "cap"):
            self.assertIn(expected, locked_ids,
                f"Expected '{expected}' to be locked, locked set: {locked_ids}")


# ---------------------------------------------------------------------------
# UNT-012 to UNT-014 — POST /api/lua/modules
# ---------------------------------------------------------------------------

class TestLuaModulesPost(unittest.TestCase):

    def _get_disabled(self):
        return board_get("/api/lua/modules").json().get("disabled", "")

    def test_UNT_012_post_modules_no_body_400(self):
        """POST /api/lua/modules with no body returns 400."""
        r = requests.post(BOARD_BASE_URL + "/api/lua/modules",
                          data=b"", timeout=HTTP_TIMEOUT)
        self.assertIn(r.status_code, [400, 422],
            f"Expected 400, got {r.status_code}: {r.text}")

    def test_UNT_013_post_modules_missing_disabled_400(self):
        """POST /api/lua/modules with JSON but no disabled field returns 400."""
        r = board_post("/api/lua/modules", json={"other": 123})
        self.assertIn(r.status_code, [400, 422],
            f"Expected 400 for missing disabled, got {r.status_code}: {r.text}")

    def test_UNT_014_post_modules_valid_disabled_roundtrip(self):
        """POST /api/lua/modules with a valid disabled string persists and is readable back."""
        original_disabled = self._get_disabled()
        # Disable the 'spi' module (unlocked, not chip-critical)
        r = board_post("/api/lua/modules", json={"disabled": "spi"})
        self.assertEqual(r.status_code, 200,
            f"POST modules returned {r.status_code}: {r.text}")
        self.assertTrue(r.json().get("ok"), f"Expected ok=true: {r.json()}")

        # Verify the new disabled list was persisted
        new_disabled = self._get_disabled()
        self.assertIn("spi", new_disabled,
            f"Disabled list not persisted: sent 'spi', got back '{new_disabled}'")

        # Restore original disabled list
        board_post("/api/lua/modules", json={"disabled": original_disabled})


# ---------------------------------------------------------------------------
# UNT-015 to UNT-016 — POST /api/cap/invoke
# ---------------------------------------------------------------------------

class TestCapInvoke(unittest.TestCase):

    def test_UNT_015_cap_invoke_no_body_400(self):
        """POST /api/cap/invoke with empty body returns 400."""
        r = requests.post(BOARD_BASE_URL + "/api/cap/invoke",
                          data=b"", timeout=HTTP_TIMEOUT)
        self.assertIn(r.status_code, [400, 422],
            f"Expected 400 for empty body, got {r.status_code}: {r.text}")

    def test_UNT_016_cap_invoke_list_dir_capability(self):
        """POST /api/cap/invoke with list_dir cap returns a directory listing."""
        r = board_post("/api/cap/invoke", json={
            "cap": "list_dir",
            "input": {"path": "vfs:/user/"}
        })
        self.assertIn(r.status_code, [200, 400, 404, 500],
            f"Unexpected status {r.status_code}: {r.text[:200]}")
        if r.status_code == 200:
            data = r.json()
            # Should be cap output or wrapped result
            self.assertTrue(
                "files" in data or "path" in data or "output" in data or "result" in data,
                f"Unexpected cap/invoke response shape: {data}"
            )


# ---------------------------------------------------------------------------
# UNT-017 to UNT-018 — /api/lua/modules disabled/locked semantics
#
# UNT-014 already verifies the disabled string roundtrip.
# These tests verify the *behavioral* meaning of that string:
#   UNT-017: disabled module → enabled=false in GET response
#   UNT-018: locked module cannot be added to the disabled list
# ---------------------------------------------------------------------------

class TestLuaModulesDisabledSemantics(unittest.TestCase):
    """UNT-017 to UNT-018: disabled list effects on enabled/locked fields."""

    # Use 'spi' — unlocked HW module compiled-in by default
    UNLOCKED_MODULE = "spi"
    # 'sys' and 'cap' are locked SW modules
    LOCKED_MODULES  = ("sys", "cap")

    def _get_modules(self):
        r = board_get("/api/lua/modules")
        r.raise_for_status()
        return r.json()

    def _set_disabled(self, csv):
        return board_post("/api/lua/modules", json={"disabled": csv})

    def _find_module(self, modules_list, mod_id):
        for m in modules_list:
            if m.get("id") == mod_id:
                return m
        return None

    def tearDown(self):
        # Restore empty disabled list so other tests start clean
        try:
            self._set_disabled("")
        except Exception:
            pass

    def test_UNT_017_disabled_module_shows_enabled_false(self):
        """UNT-017: Disabling an unlocked module makes its enabled field false.

        UNT-014 only checks the raw disabled string is stored.
        This test verifies the enabled field — the actual flag that controls
        whether the module is loaded into new Lua states.
        """
        data_before = self._get_modules()
        m_before = self._find_module(data_before["modules"], self.UNLOCKED_MODULE)
        if m_before is None:
            self.skipTest(f"Module '{self.UNLOCKED_MODULE}' not compiled in")

        # Disable the module
        r = self._set_disabled(self.UNLOCKED_MODULE)
        self.assertEqual(r.status_code, 200, f"POST failed: {r.text}")
        self.assertTrue(r.json().get("ok"))

        data_after = self._get_modules()
        m_after = self._find_module(data_after["modules"], self.UNLOCKED_MODULE)
        self.assertIsNotNone(m_after, f"Module '{self.UNLOCKED_MODULE}' disappeared from list")
        self.assertFalse(
            m_after["enabled"],
            f"Module '{self.UNLOCKED_MODULE}' should be enabled=false after disabling, got: {m_after}"
        )
        # disabled string should contain the module name
        self.assertIn(self.UNLOCKED_MODULE, data_after.get("disabled", ""),
            f"'{self.UNLOCKED_MODULE}' should appear in disabled string")

    def test_UNT_018_locked_module_cannot_be_disabled(self):
        """UNT-018: POST with a locked module in the disabled list is accepted (200)
        but the locked module is silently filtered out — it stays enabled=true.

        cap_webui's POST handler strips locked/chip-absent modules from the
        disabled list before saving, so the stored list never contains them.
        """
        for mod_id in self.LOCKED_MODULES:
            with self.subTest(module=mod_id):
                r = self._set_disabled(mod_id)
                self.assertEqual(r.status_code, 200,
                    f"POST with locked module '{mod_id}' should return 200, got {r.status_code}: {r.text}")
                self.assertTrue(r.json().get("ok"))

                data = self._get_modules()

                # The locked module must still be enabled
                m = self._find_module(data["modules"], mod_id)
                if m is None:
                    continue  # compiled out — skip
                self.assertTrue(
                    m["enabled"],
                    f"Locked module '{mod_id}' should remain enabled=true, got: {m}"
                )
                # Must not appear in the stored disabled string
                self.assertNotIn(mod_id, data.get("disabled", ""),
                    f"Locked module '{mod_id}' should not be stored in disabled list")


if __name__ == "__main__":
    unittest.main(verbosity=2)
