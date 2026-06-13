"""
WX-001, WX-002, WX-003: WeChat smoke tests (Low testability).
These tests only verify basic API availability without full WeChat login.
"""
import unittest
import requests
import sys
import os
sys.path.insert(0, os.path.dirname(__file__))
from config import BOARD_BASE_URL, HTTP_TIMEOUT


class TestWeChatSmoke(unittest.TestCase):
    """WX-001 to WX-003: WeChat API smoke tests."""

    def test_WX_001_status_without_base_url_no_crash(self):
        """WX-001: GET /status does not crash even without WeChat config."""
        r = requests.get(BOARD_BASE_URL + "/status", timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200)
        # System is running normally
        data = r.json()
        self.assertIn("wifi", data)

    def test_WX_002_qrcode_endpoint_responds(self):
        """WX-002: GET /api/wechat/qrcode responds (200 or 503)."""
        r = requests.get(BOARD_BASE_URL + "/api/wechat/qrcode", timeout=HTTP_TIMEOUT)
        self.assertIn(r.status_code, [200, 503],
            f"Expected 200/503, got {r.status_code}")
        if r.status_code == 503:
            data = r.json()
            # Should return error indicating wifi not connected or not configured
            self.assertIn("error", data)

    def test_WX_003_status_endpoint_responds(self):
        """WX-003: GET /api/wechat/status responds."""
        r = requests.get(BOARD_BASE_URL + "/api/wechat/status", timeout=HTTP_TIMEOUT)
        self.assertIn(r.status_code, [200, 404],
            f"Unexpected status: {r.status_code}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
