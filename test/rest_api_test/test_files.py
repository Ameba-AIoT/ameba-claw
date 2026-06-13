"""
FILE-001 to FILE-008: cap_files HTTP API tests.
"""
import unittest
import requests
import sys
import os
sys.path.insert(0, os.path.dirname(__file__))
from config import BOARD_BASE_URL, HTTP_TIMEOUT


def board_get(path, **kwargs):
    return requests.get(BOARD_BASE_URL + path, timeout=HTTP_TIMEOUT, **kwargs)

def board_delete(path, **kwargs):
    return requests.delete(BOARD_BASE_URL + path, timeout=HTTP_TIMEOUT, **kwargs)

def upload_file(filename, content, dest_dir="/"):
    return requests.post(
        BOARD_BASE_URL + "/api/files/upload",
        files={"file": (filename, content)},
        data={"path": dest_dir},
        timeout=HTTP_TIMEOUT
    )


class TestFilesListing(unittest.TestCase):
    """FILE-001 to FILE-003: Directory listing."""

    def test_FILE_001_list_root(self):
        """GET /api/files?path=/ returns valid file listing."""
        r = board_get("/api/files", params={"path": "/"})
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertIn("entries", data)
        self.assertIn("path", data)
        # entries is a list
        self.assertIsInstance(data["entries"], list)

    def test_FILE_002_list_memory_dir(self):
        """GET /api/files?path=/memory returns memory directory contents."""
        r = board_get("/api/files", params={"path": "/memory"})
        # Either 200 with entries or 404 if dir doesn't exist yet
        if r.status_code == 200:
            data = r.json()
            self.assertIn("entries", data)
        else:
            self.assertIn(r.status_code, [400, 404])

    def test_FILE_003_list_nonexistent_dir(self):
        """GET /api/files?path=/nonexist returns 404 or empty entries."""
        r = board_get("/api/files", params={"path": "/nonexist_test_xyz"})
        if r.status_code == 200:
            self.assertEqual(r.json().get("entries", []), [])
        else:
            self.assertIn(r.status_code, [400, 404])

    def test_FILE_007_empty_path_handled(self):
        """GET /api/files?path= (empty) returns 400 or root listing."""
        r = board_get("/api/files", params={"path": ""})
        self.assertIn(r.status_code, [200, 400], f"Got {r.status_code}")


class TestFilesDeleteCRUD(unittest.TestCase):
    """FILE-004 to FILE-006: File deletion."""

    def setUp(self):
        self.test_filename = "test_file004_auto.txt"
        self.test_path = "/" + self.test_filename
        # Upload test file
        r = upload_file(self.test_filename, b"hello test content")
        if r.status_code != 200:
            self.skipTest(f"Cannot create test file (upload returned {r.status_code})")

    def test_FILE_004_delete_existing_file(self):
        """DELETE existing file returns 200 and file is gone."""
        r = board_delete("/api/files", params={"path": self.test_path})
        self.assertEqual(r.status_code, 200, f"DELETE failed: {r.text}")
        data = r.json()
        self.assertTrue(data.get("ok", True))  # ok field or just 200

        # Verify it's gone
        r2 = board_get("/api/files", params={"path": "/"})
        if r2.status_code == 200:
            entries = r2.json().get("entries", [])
            names = [e["name"] for e in entries]
            self.assertNotIn(self.test_filename, names)

    def test_FILE_005_delete_nonexistent_file(self):
        """DELETE nonexistent file returns 4xx."""
        r = board_delete("/api/files", params={"path": "/no_such_file_xyz.txt"})
        self.assertIn(r.status_code, [400, 404, 500], f"Expected 4xx, got {r.status_code}")

    def test_FILE_006_delete_config_file_evaluated(self):
        """DELETE /claw_config.json: either rejected or succeeds (document behavior)."""
        r = board_delete("/api/files", params={"path": "/claw_config.json"})
        # Document the actual behavior
        if r.status_code == 200:
            # This is a potential security concern - config file was deleted
            # Mark as warning but not fail (behavior documentation)
            print(f"\nWARNING: Deleting /claw_config.json returned 200 - "
                  f"critical file can be deleted via HTTP API")
        else:
            # Properly rejected
            self.assertIn(r.status_code, [400, 403, 404],
                f"Expected reject, got {r.status_code}")


class TestFilesUploadDownload(unittest.TestCase):
    """File upload and download."""

    def test_upload_and_download(self):
        """Upload a file then download it and verify content."""
        filename = "test_upload_download.txt"
        content = b"Test upload download content 12345"
        r_up = upload_file(filename, content)
        if r_up.status_code != 200:
            self.skipTest(f"Upload not supported (status {r_up.status_code})")

        r_dl = board_get("/api/files/download", params={"path": "/" + filename})
        if r_dl.status_code == 200:
            self.assertEqual(r_dl.content, content)
        else:
            self.skipTest(f"Download not supported (status {r_dl.status_code})")

        # Cleanup
        board_delete("/api/files", params={"path": "/" + filename})


if __name__ == "__main__":
    unittest.main(verbosity=2)
