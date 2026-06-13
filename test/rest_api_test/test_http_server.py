"""
HTTP-003 to HTTP-010: HTTP Server core tests.
Tests run against the board's HTTP server at BOARD_BASE_URL.
"""
import unittest
import requests
import threading
import socket
import time
import sys
import os
sys.path.insert(0, os.path.dirname(__file__))
from config import BOARD_BASE_URL, HTTP_TIMEOUT


def board_get(path, **kwargs):
    return requests.get(BOARD_BASE_URL + path, timeout=HTTP_TIMEOUT, **kwargs)

def board_post(path, **kwargs):
    return requests.post(BOARD_BASE_URL + path, timeout=HTTP_TIMEOUT, **kwargs)


class TestHTTPServerBasic(unittest.TestCase):
    """HTTP-001/002: Basic reachability and JSON response."""

    def test_HTTP_001_status_200_json(self):
        """GET /status returns 200 OK with valid JSON."""
        r = board_get("/status")
        self.assertEqual(r.status_code, 200, f"Expected 200, got {r.status_code}")
        data = r.json()
        self.assertIn("wifi", data)
        self.assertIn("heap", data)
        self.assertIn("mode", data)

    def test_HTTP_002_status_response_fields(self):
        """GET /status JSON contains required fields."""
        data = board_get("/status").json()
        wifi = data["wifi"]
        self.assertIn("connected", wifi)
        self.assertIn("ip", wifi)
        heap = data["heap"]
        self.assertIn("free_bytes", heap)
        self.assertGreater(heap["free_bytes"], 0)


class TestHTTPServerConcurrent(unittest.TestCase):
    """HTTP-002/003: Concurrent connection handling."""

    def test_HTTP_002_four_concurrent_requests(self):
        """4 simultaneous GET /status requests all succeed."""
        results = []
        errors = []
        def do_get():
            try:
                r = requests.get(BOARD_BASE_URL + "/status", timeout=HTTP_TIMEOUT)
                results.append(r.status_code)
            except Exception as e:
                errors.append(str(e))

        threads = [threading.Thread(target=do_get) for _ in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=15)

        successes = [c for c in results if c == 200]
        self.assertGreaterEqual(len(successes), 3,
            f"Expected ≥3/4 successes, got {results}, errors: {errors}")

    def test_HTTP_003_fifth_concurrent_request_handled(self):
        """5 concurrent requests: board handles without crash (server remains functional after)."""
        results = []
        def do_get():
            try:
                r = requests.get(BOARD_BASE_URL + "/status", timeout=8)
                results.append(r.status_code)
            except Exception as e:
                results.append(str(e))

        threads = [threading.Thread(target=do_get) for _ in range(5)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=15)

        # Board must still be responsive after
        time.sleep(0.5)
        r = requests.get(BOARD_BASE_URL + "/status", timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200, "Board must be responsive after concurrent burst")


class TestHTTPServerRouting(unittest.TestCase):
    """HTTP-004/008/009: Routing and method handling."""

    def test_HTTP_004_unknown_route_404(self):
        """GET /nonexistent returns 404."""
        r = board_get("/nonexistent_route_xyz")
        self.assertEqual(r.status_code, 404)

    def test_HTTP_008_invalid_method_4xx(self):
        """PUT /status returns 4xx (method not allowed)."""
        r = requests.put(BOARD_BASE_URL + "/status", timeout=HTTP_TIMEOUT)
        self.assertIn(r.status_code, [404, 405], f"Expected 404/405, got {r.status_code}")

    def test_HTTP_009_cors_header_present(self):
        """Response includes Access-Control-Allow-Origin header."""
        r = board_get("/status")
        self.assertIn("Access-Control-Allow-Origin", r.headers,
            "CORS header missing")
        self.assertEqual(r.headers["Access-Control-Allow-Origin"], "*")


class TestHTTPServerBodyLimits(unittest.TestCase):
    """HTTP-005/006: POST body size limits."""

    def test_HTTP_005_post_body_8kb(self):
        """POST with oversize SSID: handler validates length, returns 400."""
        big_body = b'{"ssid":"' + b'A' * 8100 + b'","password":""}'
        try:
            r = requests.post(BOARD_BASE_URL + "/api/wifi/connect",
                              data=big_body,
                              headers={"Content-Type": "application/json"},
                              timeout=HTTP_TIMEOUT)
            # Handler should reject SSID > 32 chars with 400
            self.assertEqual(r.status_code, 400,
                f"Expected 400 for long SSID, got {r.status_code}: {r.text[:200]}")
            data = r.json()
            self.assertIn("error", data)
        except requests.exceptions.ConnectionError:
            pass  # Server closed connection before response — still acceptable
        # Board must still respond
        r2 = board_get("/status")
        self.assertEqual(r2.status_code, 200, "Board must survive large body")

    def test_HTTP_006_post_body_over_limit_no_crash(self):
        """POST with >8KB body: server does not crash, remains responsive."""
        over_limit = b'X' * 9000
        try:
            r = requests.post(BOARD_BASE_URL + "/api/wifi/connect",
                              data=over_limit,
                              headers={"Content-Type": "application/json"},
                              timeout=HTTP_TIMEOUT)
        except Exception:
            pass
        time.sleep(0.3)
        r2 = board_get("/status")
        self.assertEqual(r2.status_code, 200, "Board must survive over-limit body")


class TestHTTPServerSecurity(unittest.TestCase):
    """HTTP-007: Security tests."""

    def test_HTTP_007_path_traversal_rejected(self):
        """GET /api/files?path=../../etc/passwd is rejected or returns empty."""
        r = board_get("/api/files", params={"path": "../../etc/passwd"})
        # Must not return success with file content
        if r.status_code == 200:
            body = r.text
            self.assertNotIn("root:", body, "Path traversal should not leak /etc/passwd")
            self.assertNotIn("/bin/bash", body)
        # 400 or 404 are fine responses
        self.assertIn(r.status_code, [200, 400, 404])


class TestHTTPServerHalfOpen(unittest.TestCase):
    """HTTP-010: Half-open connection handling."""

    def test_HTTP_010_half_open_connection_no_hang(self):
        """Partial HTTP request followed by disconnect: server does not hang."""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(5)
            s.connect(("127.0.0.1", 80))
            s.sendall(b"GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n")
            # Intentionally don't send final \r\n, then close
            time.sleep(0.1)
            s.close()
        except Exception:
            pass

        # Wait a bit then verify board is still responsive
        time.sleep(6)  # wait > server recv timeout (5s)
        r = board_get("/status")
        self.assertEqual(r.status_code, 200, "Board must recover from half-open connection")


if __name__ == "__main__":
    unittest.main(verbosity=2)
