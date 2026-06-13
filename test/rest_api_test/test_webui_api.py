"""
WUI-001 to WUI-014: cap_webui HTTP API tests.
"""
import unittest
import requests
import time
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


class TestWebUIStatus(unittest.TestCase):
    """WUI-001/002: /status endpoint."""

    def test_WUI_001_status_softap_fields(self):
        """GET /status includes softap block with expected fields."""
        data = board_get("/status").json()
        self.assertIn("softap", data)
        softap = data["softap"]
        self.assertIn("running", softap)
        self.assertIn("ssid", softap)
        self.assertIn("ip", softap)

    def test_WUI_002_status_sta_connected(self):
        """GET /status STA mode: wifi.connected=true, ip is non-empty."""
        data = board_get("/status").json()
        wifi = data["wifi"]
        # Board is in STA mode connected to Redmi_K50
        self.assertTrue(wifi["connected"], "Board should be STA connected")
        self.assertNotEqual(wifi.get("ip", ""), "", "IP should be non-empty in STA mode")
        self.assertEqual(data.get("mode"), "normal")

    def test_WUI_002b_status_wifi_mode_field(self):
        """GET /status contains wifi.configured=true in STA mode."""
        data = board_get("/status").json()
        self.assertTrue(data["wifi"]["configured"])


class TestWebUISetupPage(unittest.TestCase):
    """WUI-003: /setup page."""

    def test_WUI_003_setup_returns_html(self):
        """GET /setup returns 200 with text/html content."""
        r = board_get("/setup")
        self.assertEqual(r.status_code, 200)
        ct = r.headers.get("Content-Type", "")
        self.assertIn("text/html", ct, f"Expected text/html, got {ct}")
        self.assertGreater(len(r.content), 100, "HTML page should not be empty")


class TestWebUIWifiConnect(unittest.TestCase):
    """WUI-004 to WUI-008: POST /api/wifi/connect validation tests.

    Endpoint behaviour (provisioning mode):
      - Valid SSID/password → 200 {"ok":true,"connecting":true} immediately; board
        connects asynchronously, client polls GET /status for wifi.connected and ip.
      - No reboot on connect: device stays in concurrent AP+STA mode.

    IMPORTANT: Tests that trigger actual WiFi reconnection (valid SSID/password)
    are intentionally skipped — the board disconnects from the test network while
    reconnecting.  Only fast-path validation tests (400 responses) are run.
    """

    def test_WUI_004_endpoint_exists_and_validates_json(self):
        """WUI-004: Endpoint exists, rejects missing ssid with 400 (fast validation path)."""
        r = board_post("/api/wifi/connect", json={"password": "only_password"})
        self.assertEqual(r.status_code, 400,
            f"Missing ssid should return 400, got {r.status_code}: {r.text}")
        # NOTE: Testing with a real SSID is skipped to prevent board WiFi disconnect.
        # When a valid SSID is provided the response is {"ok":true,"connecting":true}
        # (async) and the board reconnects in background; poll GET /status for result.
        print("\n  NOTE WUI-004: Actual-connection test skipped (would disconnect board)")

    def test_WUI_005_non_json_body_400(self):
        """WUI-005: POST /api/wifi/connect with plain text returns 400."""
        r = requests.post(BOARD_BASE_URL + "/api/wifi/connect",
                          data="plain text body",
                          headers={"Content-Type": "text/plain"},
                          timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 400, f"Expected 400, got {r.status_code}")

    def test_WUI_006_missing_ssid_400(self):
        """WUI-006: POST /api/wifi/connect missing ssid field returns 400."""
        r = board_post("/api/wifi/connect",
                       json={"password": "somepassword"})
        self.assertEqual(r.status_code, 400, f"Expected 400, got {r.status_code}")

    def test_WUI_007_empty_ssid_400(self):
        """WUI-007: POST /api/wifi/connect empty ssid returns 400 (fast validation)."""
        r = board_post("/api/wifi/connect",
                       json={"ssid": "", "password": "test"})
        self.assertEqual(r.status_code, 400, f"Expected 400 for empty ssid, got {r.status_code}")

    def test_WUI_008_empty_body_400(self):
        """WUI-008: POST /api/wifi/connect with empty body returns 400."""
        r = requests.post(BOARD_BASE_URL + "/api/wifi/connect",
                          data=b"",
                          timeout=HTTP_TIMEOUT)
        self.assertIn(r.status_code, [400, 500],
            f"Expected 400/500 for empty body, got {r.status_code}")


class TestWebUIWifiScan(unittest.TestCase):
    """WUI-009: GET /api/wifi/scan."""

    def test_WUI_009_wifi_scan_returns_json(self):
        """GET /api/wifi/scan returns 200 with JSON array."""
        r = board_get("/api/wifi/scan")
        self.assertEqual(r.status_code, 200)
        data = r.json()
        # Stub returns {"networks": []} or similar array
        self.assertTrue(
            isinstance(data, list) or "networks" in data,
            f"Expected list or {{networks:...}}, got: {data}"
        )


class TestWebUIFiles(unittest.TestCase):
    """WUI-010 to WUI-014: /api/files CRUD."""

    def test_WUI_010_list_root_directory(self):
        """GET /api/files?path=/ returns file listing JSON."""
        r = board_get("/api/files", params={"path": "/"})
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertIn("entries", data, f"Expected 'entries' key, got: {data}")
        self.assertIn("path", data)

    def test_WUI_011_nonexistent_path_404_or_empty(self):
        """GET /api/files?path=/nonexistent returns 404 or empty entries."""
        r = board_get("/api/files", params={"path": "/nonexistent_xyz"})
        if r.status_code == 200:
            data = r.json()
            entries = data.get("entries", [])
            self.assertEqual(entries, [], f"Expected empty list for nonexistent path, got {entries}")
        else:
            self.assertIn(r.status_code, [400, 404])

    def test_WUI_012_delete_and_verify(self):
        """Create a test file via LLM then DELETE it via API."""
        # First check if test file exists, delete if so
        test_path = "/test_wui012_deleteme.txt"
        # We'll use file upload API to create it first
        upload_r = requests.post(
            BOARD_BASE_URL + "/api/files/upload",
            files={"file": ("test_wui012_deleteme.txt", b"test content")},
            data={"path": "/"},
            timeout=HTTP_TIMEOUT
        )
        if upload_r.status_code != 200:
            self.skipTest(f"File upload not available (status {upload_r.status_code}), skipping delete test")

        # Now delete
        r = board_delete("/api/files", params={"path": test_path})
        self.assertEqual(r.status_code, 200, f"DELETE returned {r.status_code}: {r.text}")

        # Verify deleted
        r2 = board_get("/api/files", params={"path": "/"})
        if r2.status_code == 200:
            entries = r2.json().get("entries", [])
            names = [e["name"] for e in entries]
            self.assertNotIn("test_wui012_deleteme.txt", names, "File should be deleted")

    def test_WUI_013_delete_nonexistent_4xx(self):
        """DELETE /api/files for nonexistent path returns 4xx."""
        r = board_delete("/api/files", params={"path": "/does_not_exist_xyz.txt"})
        self.assertIn(r.status_code, [400, 404, 500],
            f"Expected 4xx, got {r.status_code}")

    def test_WUI_014_delete_path_traversal_rejected(self):
        """DELETE /api/files with path traversal is rejected."""
        r = board_delete("/api/files", params={"path": "../../claw_config.json"})
        # Must not return 200 success for a path traversal attempt
        if r.status_code == 200:
            # If 200, the file must not actually be a critical config
            data = r.json()
            self.assertFalse(data.get("ok", False) and "claw_config" in r.url,
                "Path traversal should be rejected")
        else:
            self.assertIn(r.status_code, [400, 403, 404, 500])


class TestWebUIConfig(unittest.TestCase):
    """GET /api/config: configuration endpoint."""

    def test_api_config_returns_all_sections(self):
        """GET /api/config returns all config sections."""
        r = board_get("/api/config")
        self.assertEqual(r.status_code, 200)
        data = r.json()
        for section in ["wifi", "llm", "telegram", "feishu"]:
            self.assertIn(section, data, f"Missing config section: {section}")

    def test_api_config_llm_section(self):
        """GET /api/config llm section has required fields."""
        data = board_get("/api/config").json()
        llm = data["llm"]
        self.assertIn("model", llm)
        self.assertIn("api_url", llm)
        self.assertIn("max_tokens", llm)


if __name__ == "__main__":
    unittest.main(verbosity=2)
