"""
FILE-EXT-001 to FILE-EXT-015: Enhanced file management API tests.

Covers new features introduced in this session:
  - mtime field in directory listing
  - size=-1 for directories, size=0 for empty files
  - GET /api/files/download for directories returns a valid uncompressed ZIP
  - DELETE /api/files on directories performs recursive deletion
  - Error / security cases for the merged download endpoint
"""
import io
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

def mkdir(path):
    """Create a directory via POST /api/files/mkdir. Returns True on success."""
    r = requests.post(BOARD_BASE_URL + "/api/files/mkdir",
                      params={"path": path}, timeout=HTTP_TIMEOUT)
    return r.status_code == 200 and r.json().get("ok", False)

def list_root_names():
    r = board_get("/api/files", params={"path": "/"})
    if r.status_code != 200:
        return []
    return [e["name"] for e in r.json().get("entries", [])]

def is_valid_zip(data: bytes) -> bool:
    """Minimal check: PK local-file signature at start, EOCD signature in last 22 bytes."""
    return len(data) >= 22 and data[:2] == b'PK' and b'PK\x05\x06' in data[-65536:]


# ---------------------------------------------------------------------------
# FILE-EXT-001 to FILE-EXT-004 — New fields in directory listing
# ---------------------------------------------------------------------------

class TestListingNewFields(unittest.TestCase):
    """Verify mtime and size fields added to /api/files responses."""

    def _entries(self, path="/"):
        r = board_get("/api/files", params={"path": path})
        self.assertEqual(r.status_code, 200)
        return r.json().get("entries", [])

    def test_FILE_EXT_001_entries_have_mtime_field(self):
        """All listing entries carry an mtime field (numeric)."""
        entries = self._entries("/")
        if not entries:
            self.skipTest("Root directory is empty")
        for e in entries:
            self.assertIn("mtime", e,
                f"Entry {e.get('name')} missing mtime field")
            self.assertIsInstance(e["mtime"], (int, float),
                f"mtime should be numeric for {e.get('name')}, got {type(e['mtime'])}")

    def test_FILE_EXT_002_directory_entries_have_size_minus_one(self):
        """Directory entries report size=-1 (not a byte count)."""
        entries = self._entries("/")
        dirs = [e for e in entries if e.get("type") == "dir"]
        if not dirs:
            self.skipTest("No subdirectories in root")
        for d in dirs:
            self.assertEqual(d.get("size"), -1,
                f"Dir {d['name']} should have size=-1, got {d.get('size')}")

    def test_FILE_EXT_003_empty_file_has_size_zero(self):
        """Uploading a 0-byte file: listing shows size=0, not -1 or None."""
        fname = "test_empty_ext003.txt"
        r = upload_file(fname, b"")
        if r.status_code != 200:
            self.skipTest(f"Upload returned {r.status_code}")
        try:
            entries = self._entries("/")
            match = next((e for e in entries if e["name"] == fname), None)
            self.assertIsNotNone(match, f"{fname} not found after upload")
            self.assertEqual(match["size"], 0,
                f"Empty file should have size=0, got {match.get('size')}")
        finally:
            board_delete("/api/files", params={"path": "/" + fname})

    def test_FILE_EXT_004_nonzero_file_size_matches_upload(self):
        """Listing size equals the number of bytes uploaded."""
        fname = "test_size_ext004.txt"
        content = b"Hello, size check! " * 10   # 190 bytes exactly
        r = upload_file(fname, content)
        if r.status_code != 200:
            self.skipTest(f"Upload returned {r.status_code}")
        try:
            entries = self._entries("/")
            match = next((e for e in entries if e["name"] == fname), None)
            self.assertIsNotNone(match)
            self.assertEqual(match["size"], len(content),
                f"Expected size={len(content)}, got {match.get('size')}")
        finally:
            board_delete("/api/files", params={"path": "/" + fname})


# ---------------------------------------------------------------------------
# FILE-EXT-005 to FILE-EXT-008 — Merged download endpoint: file branch
# ---------------------------------------------------------------------------

class TestDownloadFileBranch(unittest.TestCase):
    """GET /api/files/download on a regular file (pre-existing behaviour, merged endpoint)."""

    TEST_FILE = "test_dl_ext005.txt"
    TEST_CONTENT = b"download-test-content-abc123"

    @classmethod
    def setUpClass(cls):
        r = upload_file(cls.TEST_FILE, cls.TEST_CONTENT)
        cls.upload_ok = (r.status_code == 200)

    @classmethod
    def tearDownClass(cls):
        board_delete("/api/files", params={"path": "/" + cls.TEST_FILE})

    def test_FILE_EXT_005_download_file_content_exact(self):
        """GET /api/files/download returns exact uploaded bytes."""
        if not self.upload_ok:
            self.skipTest("Upload failed in setUpClass")
        r = board_get("/api/files/download", params={"path": "/" + self.TEST_FILE})
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.content, self.TEST_CONTENT)

    def test_FILE_EXT_006_download_missing_file_404(self):
        """GET /api/files/download for nonexistent path → 404."""
        r = board_get("/api/files/download", params={"path": "/no_such_file_xyz.bin"})
        self.assertEqual(r.status_code, 404, f"Expected 404, got {r.status_code}")

    def test_FILE_EXT_007_download_no_path_param_400(self):
        """GET /api/files/download with no path parameter → 400."""
        r = board_get("/api/files/download")
        self.assertEqual(r.status_code, 400, f"Expected 400, got {r.status_code}")

    def test_FILE_EXT_008_download_path_traversal_rejected(self):
        """GET /api/files/download with '..' in path is rejected (400/403/404)."""
        r = board_get("/api/files/download", params={"path": "../../etc/passwd"})
        self.assertIn(r.status_code, [400, 403, 404],
            f"Path traversal should be rejected, got {r.status_code}")


# ---------------------------------------------------------------------------
# FILE-EXT-009 to FILE-EXT-012 — Merged download endpoint: directory → ZIP
# ---------------------------------------------------------------------------

class TestDownloadDirAsZip(unittest.TestCase):
    """GET /api/files/download on a directory returns a valid uncompressed ZIP."""

    TEST_DIR = "test_dl_dir_ext009"
    FILE_A = "alpha.txt"
    FILE_B = "beta.txt"
    CONTENT_A = b"content of alpha"
    CONTENT_B = b"content of beta"

    @classmethod
    def setUpClass(cls):
        mkdir("/" + cls.TEST_DIR)
        r1 = upload_file(cls.FILE_A, cls.CONTENT_A, "/" + cls.TEST_DIR)
        r2 = upload_file(cls.FILE_B, cls.CONTENT_B, "/" + cls.TEST_DIR)
        cls.setup_ok = (r1.status_code == 200 and r2.status_code == 200)

    @classmethod
    def tearDownClass(cls):
        board_delete("/api/files", params={"path": "/" + cls.TEST_DIR})

    def test_FILE_EXT_009_download_dir_status_200(self):
        """GET /api/files/download?path=<dir> returns 200."""
        if not self.setup_ok:
            self.skipTest("Test directory setup failed")
        r = board_get("/api/files/download", params={"path": "/" + self.TEST_DIR})
        self.assertEqual(r.status_code, 200,
            f"Expected 200, got {r.status_code}: {r.text[:200]}")

    def test_FILE_EXT_010_download_dir_content_type_zip(self):
        """Directory download Content-Type is application/zip."""
        if not self.setup_ok:
            self.skipTest("Test directory setup failed")
        r = board_get("/api/files/download", params={"path": "/" + self.TEST_DIR})
        self.assertEqual(r.status_code, 200)
        ct = r.headers.get("Content-Type", "")
        self.assertIn("zip", ct.lower(),
            f"Expected application/zip, got: {ct}")

    def test_FILE_EXT_011_downloaded_zip_is_structurally_valid(self):
        """ZIP response has PK signature and End-of-Central-Directory record."""
        if not self.setup_ok:
            self.skipTest("Test directory setup failed")
        r = board_get("/api/files/download", params={"path": "/" + self.TEST_DIR})
        self.assertEqual(r.status_code, 200)
        self.assertTrue(is_valid_zip(r.content),
            f"Not a valid ZIP ({len(r.content)} bytes, header: {r.content[:4].hex()})")

    def test_FILE_EXT_012_zip_contains_correct_files_and_content(self):
        """ZIP contains both uploaded files with correct content."""
        if not self.setup_ok:
            self.skipTest("Test directory setup failed")
        r = board_get("/api/files/download", params={"path": "/" + self.TEST_DIR})
        self.assertEqual(r.status_code, 200)
        import zipfile
        try:
            with zipfile.ZipFile(io.BytesIO(r.content)) as zf:
                names = zf.namelist()
                self.assertIn(self.FILE_A, names,
                    f"Expected {self.FILE_A} in ZIP, got: {names}")
                self.assertIn(self.FILE_B, names,
                    f"Expected {self.FILE_B} in ZIP, got: {names}")
                self.assertEqual(zf.read(self.FILE_A), self.CONTENT_A,
                    f"{self.FILE_A} content mismatch")
                self.assertEqual(zf.read(self.FILE_B), self.CONTENT_B,
                    f"{self.FILE_B} content mismatch")
        except zipfile.BadZipFile as e:
            self.fail(f"ZIP parse failed: {e} — raw bytes[:16]: {r.content[:16].hex()}")


# ---------------------------------------------------------------------------
# FILE-EXT-013 to FILE-EXT-015 — Recursive directory deletion
# ---------------------------------------------------------------------------

class TestDirRecursiveDelete(unittest.TestCase):
    """DELETE /api/files on a directory removes it and all contents."""

    def test_FILE_EXT_013_delete_empty_directory(self):
        """DELETE an empty directory returns ok=true and dir disappears from listing."""
        dirname = "test_del_empty_ext013"
        if not mkdir("/" + dirname):
            self.skipTest("Cannot create test directory")
        # Upload a file then delete it so dir is empty
        r = upload_file("tmp.txt", b"x", "/" + dirname)
        if r.status_code != 200:
            board_delete("/api/files", params={"path": "/" + dirname})
            self.skipTest("Cannot upload into test directory")
        board_delete("/api/files", params={"path": f"/{dirname}/tmp.txt"})

        r = board_delete("/api/files", params={"path": "/" + dirname})
        self.assertEqual(r.status_code, 200,
            f"Delete empty dir failed: {r.status_code} {r.text}")
        self.assertTrue(r.json().get("ok"),
            f"Expected ok=true: {r.json()}")
        self.assertNotIn(dirname, list_root_names(),
            "Directory still visible in listing after delete")

    def test_FILE_EXT_014_delete_nonempty_directory_recursively(self):
        """DELETE a directory that contains files removes everything."""
        dirname = "test_del_nonempty_ext014"
        if not mkdir("/" + dirname):
            self.skipTest("Cannot create test directory")
        upload_file("f1.txt", b"content one", "/" + dirname)
        upload_file("f2.txt", b"content two", "/" + dirname)
        r_check = board_get("/api/files", params={"path": "/" + dirname})
        if r_check.status_code != 200 or not r_check.json().get("entries"):
            self.skipTest("Cannot verify test directory was created")

        r = board_delete("/api/files", params={"path": "/" + dirname})
        self.assertEqual(r.status_code, 200,
            f"Recursive delete returned {r.status_code}: {r.text}")
        self.assertTrue(r.json().get("ok"),
            f"Expected ok=true: {r.json()}")
        self.assertNotIn(dirname, list_root_names(),
            "Directory still visible after recursive delete")

    def test_FILE_EXT_015_files_inside_deleted_dir_are_inaccessible(self):
        """After recursive delete the files inside can no longer be downloaded."""
        dirname = "test_del_access_ext015"
        fname = "secret.txt"
        if not mkdir("/" + dirname):
            self.skipTest("Cannot create test directory")
        r = upload_file(fname, b"secret content", "/" + dirname)
        if r.status_code != 200:
            board_delete("/api/files", params={"path": "/" + dirname})
            self.skipTest("Cannot create test file")

        board_delete("/api/files", params={"path": "/" + dirname})

        r2 = board_get("/api/files/download",
                       params={"path": f"/{dirname}/{fname}"})
        self.assertIn(r2.status_code, [400, 404],
            f"Deleted file should not be downloadable, got {r2.status_code}")


# ---------------------------------------------------------------------------
# FILE-EXT-016 to FILE-EXT-019 — POST /api/files/mkdir (new endpoint)
# ---------------------------------------------------------------------------

class TestFileMkdir(unittest.TestCase):
    """POST /api/files/mkdir creates directories."""

    def test_FILE_EXT_016_mkdir_creates_directory(self):
        """POST /api/files/mkdir creates a new directory visible in listing."""
        dirname = "test_mkdir_ext016"
        try:
            r = requests.post(BOARD_BASE_URL + "/api/files/mkdir",
                              params={"path": "/" + dirname}, timeout=HTTP_TIMEOUT)
            self.assertEqual(r.status_code, 200,
                f"mkdir returned {r.status_code}: {r.text}")
            self.assertTrue(r.json().get("ok"), f"Expected ok=true: {r.json()}")
            # Verify directory appears in listing
            names = list_root_names()
            self.assertIn(dirname, names,
                f"New directory not visible in listing: {names}")
        finally:
            board_delete("/api/files", params={"path": "/" + dirname})

    def test_FILE_EXT_017_mkdir_duplicate_fails(self):
        """POST /api/files/mkdir on an existing path returns non-200."""
        dirname = "test_mkdir_dup_ext017"
        try:
            requests.post(BOARD_BASE_URL + "/api/files/mkdir",
                          params={"path": "/" + dirname}, timeout=HTTP_TIMEOUT)
            r = requests.post(BOARD_BASE_URL + "/api/files/mkdir",
                              params={"path": "/" + dirname}, timeout=HTTP_TIMEOUT)
            self.assertNotEqual(r.status_code, 200,
                f"Duplicate mkdir should fail, got 200: {r.text}")
        finally:
            board_delete("/api/files", params={"path": "/" + dirname})

    def test_FILE_EXT_018_mkdir_path_traversal_rejected(self):
        """POST /api/files/mkdir with '..' in path is rejected."""
        r = requests.post(BOARD_BASE_URL + "/api/files/mkdir",
                          params={"path": "../../hack"}, timeout=HTTP_TIMEOUT)
        self.assertIn(r.status_code, [400, 403],
            f"Traversal mkdir should be rejected, got {r.status_code}: {r.text}")

    def test_FILE_EXT_019_upload_to_nonexistent_dir_returns_404(self):
        """POST /api/files/upload to a non-existent directory returns 404 (not 500)."""
        r = upload_file("file.txt", b"content", "/nonexistent_dir_xyz")
        self.assertEqual(r.status_code, 404,
            f"Expected 404 for upload to missing dir, got {r.status_code}: {r.text}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
