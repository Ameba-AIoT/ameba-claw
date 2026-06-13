"""
FS-001, FS-004, FS-008: Feishu webhook tests.
FS-001: Webhook receives message and processes it.
FS-004: Malformed JSON returns 400.
FS-008: No signature verification (security audit).
"""
import unittest
import requests
import sys
import os
sys.path.insert(0, os.path.dirname(__file__))
from config import BOARD_BASE_URL, HTTP_TIMEOUT

FEISHU_PATH = "/feishu"


def post_feishu(body, content_type="application/json", **kwargs):
    if isinstance(body, dict):
        import json
        body = json.dumps(body)
    return requests.post(
        BOARD_BASE_URL + FEISHU_PATH,
        data=body if isinstance(body, bytes) else body.encode(),
        headers={"Content-Type": content_type},
        timeout=HTTP_TIMEOUT,
        **kwargs
    )


class TestFeishuWebhook(unittest.TestCase):

    def test_FS_001_challenge_verification(self):
        """Feishu URL challenge verification: returns challenge token."""
        r = post_feishu({"challenge": "test_challenge_token_12345"})
        self.assertIn(r.status_code, [200], f"Expected 200, got {r.status_code}: {r.text}")
        try:
            data = r.json()
            self.assertEqual(data.get("challenge"), "test_challenge_token_12345",
                f"Challenge response mismatch: {data}")
        except Exception:
            # Some implementations return 200 with plain text
            self.assertIn("test_challenge_token_12345", r.text)

    def test_FS_001b_normal_message_accepted(self):
        """Feishu normal message event returns 200."""
        msg = {
            "event": {
                "message": {
                    "message_type": "text",
                    "chat_id": "oc_test_chat_001",
                    "message_id": "msg_test_001",
                    "content": '{"text":"Hello from test"}'
                },
                "sender": {
                    "sender_id": {"open_id": "ou_test_user_001"}
                }
            }
        }
        r = post_feishu(msg)
        self.assertIn(r.status_code, [200], f"Expected 200, got {r.status_code}: {r.text}")

    def test_FS_004_malformed_json_400(self):
        """Feishu webhook with malformed JSON returns 400."""
        r = post_feishu("{invalid json :::}", content_type="application/json")
        self.assertIn(r.status_code, [400], f"Expected 400, got {r.status_code}: {r.text}")

    def test_FS_004b_empty_body_handled(self):
        """Feishu webhook with empty body is handled gracefully."""
        r = post_feishu("")
        self.assertIn(r.status_code, [200, 400, 500])
        # Server must still be responsive
        r2 = requests.get(BOARD_BASE_URL + "/status", timeout=HTTP_TIMEOUT)
        self.assertEqual(r2.status_code, 200)

    def test_FS_008_no_signature_bypass_audit(self):
        """FS-008: Document whether signature verification is implemented."""
        msg = {
            "event": {
                "message": {
                    "message_type": "text",
                    "chat_id": "oc_attacker_chat",
                    "message_id": "msg_fake_001",
                    "content": '{"text":"Unsigned webhook test"}'
                },
                "sender": {"sender_id": {"open_id": "ou_attacker"}}
            }
        }
        r = post_feishu(msg)  # No X-Lark-Signature header
        if r.status_code == 200:
            print(f"\nSECURITY NOTE: Feishu webhook accepts unsigned requests "
                  f"(no signature verification). "
                  f"FS-008 identifies this as a potential security gap.")
        elif r.status_code in [401, 403]:
            print(f"\nFS-008: Feishu webhook correctly rejects unsigned requests (status {r.status_code})")
        # Not failing - this test documents behavior


if __name__ == "__main__":
    unittest.main(verbosity=2)
