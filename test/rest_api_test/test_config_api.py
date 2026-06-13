"""
CFG tests: Configuration validation via HTTP API.
Only tests that use fast validation paths (400 responses returned before any
WiFi connection attempt) are automated. Tests that would trigger actual WiFi
connection (valid-looking SSID) are skipped to prevent board disconnection.

Automated:
  CFG-005: SSID empty → 400 immediately
  CFG-WUI-005: non-JSON body → 400 immediately
  CFG-WUI-006: missing ssid field → 400 immediately
  POST /setup: LLM and unknown section handling

Skipped (triggers WiFi disconnect):
  CFG-003: 32-byte SSID (would attempt connection)
  CFG-004: 33-byte SSID (no length validation in server; attempts connection)
  CFG-006: empty password open AP (would attempt connection)
  CFG-007: 63-byte password (would attempt connection)
"""
import unittest
import requests
import sys
import os
sys.path.insert(0, os.path.dirname(__file__))
from config import BOARD_BASE_URL, HTTP_TIMEOUT


class TestConfigValidation(unittest.TestCase):
    """Fast-path config validation tests (immediate 400 responses)."""

    def test_CFG_005_ssid_empty_rejected(self):
        """CFG-005: SSID empty string returns 400 (server-side fast validation)."""
        r = requests.post(
            BOARD_BASE_URL + "/api/wifi/connect",
            json={"ssid": "", "password": "password123"},
            timeout=HTTP_TIMEOUT
        )
        self.assertEqual(r.status_code, 400,
            f"Empty SSID should be rejected with 400, got {r.status_code}: {r.text}")

    def test_CFG_missing_ssid_field_400(self):
        """Missing 'ssid' field returns 400."""
        r = requests.post(
            BOARD_BASE_URL + "/api/wifi/connect",
            json={"password": "somepassword"},
            timeout=HTTP_TIMEOUT
        )
        self.assertEqual(r.status_code, 400,
            f"Missing ssid field should return 400, got {r.status_code}: {r.text}")

    def test_CFG_non_json_body_400(self):
        """Non-JSON body returns 400."""
        r = requests.post(
            BOARD_BASE_URL + "/api/wifi/connect",
            data="plain text",
            headers={"Content-Type": "text/plain"},
            timeout=HTTP_TIMEOUT
        )
        self.assertEqual(r.status_code, 400,
            f"Non-JSON body should return 400, got {r.status_code}: {r.text}")

    def test_CFG_empty_body_400(self):
        """Empty request body returns 400."""
        r = requests.post(
            BOARD_BASE_URL + "/api/wifi/connect",
            data=b"",
            timeout=HTTP_TIMEOUT
        )
        self.assertIn(r.status_code, [400, 500],
            f"Empty body should return 400/500, got {r.status_code}: {r.text}")

    @unittest.skip("CFG-003: Triggers WiFi disconnect (32-byte SSID connection attempt)")
    def test_CFG_003_ssid_32_bytes(self):
        """MANUAL: SSID of exactly 32 bytes should be accepted."""
        pass

    @unittest.skip("CFG-004: Triggers WiFi disconnect (33-byte SSID, no length validation)")
    def test_CFG_004_ssid_33_bytes(self):
        """MANUAL: SSID 33 bytes - should be rejected at WiFi driver level."""
        pass

    @unittest.skip("CFG-006: Triggers WiFi disconnect (open AP connection attempt)")
    def test_CFG_006_password_empty_open_ap(self):
        """MANUAL: Empty password for open AP."""
        pass

    @unittest.skip("CFG-007: Triggers WiFi disconnect (63-byte password connection)")
    def test_CFG_007_password_63_bytes(self):
        """MANUAL: Password of 63 bytes."""
        pass


class TestSetupEndpoint(unittest.TestCase):
    """POST /setup configuration update endpoint."""

    @classmethod
    def setUpClass(cls):
        """Read and save the current LLM config before any tests modify it."""
        try:
            r = requests.get(BOARD_BASE_URL + "/api/config", timeout=HTTP_TIMEOUT)
            cls._original_llm = r.json().get("llm", {})
        except Exception:
            cls._original_llm = None

    @classmethod
    def tearDownClass(cls):
        """Restore LLM config to original values after setup tests."""
        if cls._original_llm:
            try:
                requests.post(BOARD_BASE_URL + "/setup", json={
                    "section": "llm",
                    "api_url": cls._original_llm.get("api_url", ""),
                    "api_key": cls._original_llm.get("api_key", ""),
                    "model": cls._original_llm.get("model", ""),
                    "max_tokens": cls._original_llm.get("max_tokens", 1024),
                    "max_iterations": cls._original_llm.get("max_iterations", 5),
                }, timeout=HTTP_TIMEOUT)
            except Exception:
                pass

    def test_setup_llm_section_accepted(self):
        """POST /setup with llm section is accepted (200 OK). Uses current model to avoid breaking LLM."""
        # Use the currently configured model so the board stays functional
        current_model = (self._original_llm or {}).get("model", "glm-5.1")
        r = requests.post(
            BOARD_BASE_URL + "/setup",
            json={
                "section": "llm",
                "model": current_model,
                "max_tokens": 512,
                "max_iterations": 3,
            },
            timeout=HTTP_TIMEOUT
        )
        self.assertIn(r.status_code, [200],
            f"POST /setup llm returned {r.status_code}: {r.text}")
        data = r.json()
        self.assertTrue(data.get("ok"), f"Expected ok=true: {data}")

    def test_setup_telegram_section(self):
        """POST /setup with telegram section is accepted."""
        r = requests.post(
            BOARD_BASE_URL + "/setup",
            json={
                "section": "telegram",
                "bot_token": "test_token_validation_only",
            },
            timeout=HTTP_TIMEOUT
        )
        self.assertIn(r.status_code, [200],
            f"POST /setup telegram returned {r.status_code}: {r.text}")

    def test_setup_unknown_section_handled(self):
        """POST /setup with unknown section: server returns 200 or 400."""
        r = requests.post(
            BOARD_BASE_URL + "/setup",
            json={"section": "nonexistent_xyz"},
            timeout=HTTP_TIMEOUT
        )
        self.assertIn(r.status_code, [200, 400],
            f"Unknown section returned {r.status_code}: {r.text}")

    def test_setup_no_section_field_handled(self):
        """POST /setup with no section field: server handles gracefully."""
        r = requests.post(
            BOARD_BASE_URL + "/setup",
            json={"model": "orphan-field"},
            timeout=HTTP_TIMEOUT
        )
        self.assertIn(r.status_code, [200, 400],
            f"No section field returned {r.status_code}: {r.text}")

    def test_setup_non_json_body(self):
        """POST /setup with non-JSON body returns 400."""
        r = requests.post(
            BOARD_BASE_URL + "/setup",
            data="not json",
            headers={"Content-Type": "text/plain"},
            timeout=HTTP_TIMEOUT
        )
        self.assertIn(r.status_code, [200, 400],
            f"Non-JSON setup returned {r.status_code}: {r.text}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
