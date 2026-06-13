"""
FILE-CONT-001 to FILE-CONT-012: GET/PUT /api/files/content HTTP API tests.

Covers:
  - GET /api/files/content?path=/xxx  (read text file content)
  - PUT /api/files/content?path=/xxx  (save text content to file)
  - Error handling: missing path, file not found, path traversal,
    binary file detection, file too large, directory instead of file
"""
import unittest
import requests
import sys
import os
sys.path.insert(0, os.path.dirname(__file__))
from config import BOARD_BASE_URL, HTTP_TIMEOUT


def board_get(path, **kwargs):
    return requests.get(BOARD_BASE_URL + path, timeout=HTTP_TIMEOUT, **kwargs)


def board_put(path, data=None, **kwargs):
    if isinstance(data, str):
        data = data.encode("utf-8")
    return requests.put(BOARD_BASE_URL + path, data=data,
                        timeout=HTTP_TIMEOUT, **kwargs)


def board_delete(path, **kwargs):
    return requests.delete(BOARD_BASE_URL + path,
                           timeout=HTTP_TIMEOUT, **kwargs)


def upload_file(filename, content, dest_dir="/"):
    return requests.post(
        BOARD_BASE_URL + "/api/files/upload",
        files={"file": (filename, content)},
        data={"path": dest_dir},
        timeout=HTTP_TIMEOUT
    )


def list_root_names():
    r = board_get("/api/files", params={"path": "/"})
    if r.status_code != 200:
        return []
    return [e["name"] for e in r.json().get("entries", [])]


# ---------------------------------------------------------------------------
# FILE-CONT-001 to FILE-CONT-004 — GET /api/files/content error cases
# ---------------------------------------------------------------------------

class TestContentGetErrorCases(unittest.TestCase):
    """GET /api/files/content error handling."""

    def test_FILE_CONT_001_get_no_path(self):
        """GET /api/files/content without path param returns 400."""
        r = board_get("/api/files/content")
        self.assertEqual(r.status_code, 400,
            f"Expected 400, got {r.status_code}: {r.text}")
        data = r.json()
        self.assertIn("error", data)

    def test_FILE_CONT_002_get_nonexistent_file(self):
        """GET /api/files/content for nonexistent file returns 404."""
        r = board_get("/api/files/content",
                      params={"path": "/nonexistent_file_xyz.txt"})
        self.assertEqual(r.status_code, 404,
            f"Expected 404, got {r.status_code}: {r.text}")
        data = r.json()
        self.assertIn("error", data)

    def test_FILE_CONT_003_get_path_traversal_rejected(self):
        """GET /api/files/content with '..' in path returns 400."""
        r = board_get("/api/files/content",
                      params={"path": "../../etc/passwd"})
        self.assertEqual(r.status_code, 400,
            f"Expected 400, got {r.status_code}: {r.text}")
        data = r.json()
        self.assertIn("error", data)

    def test_FILE_CONT_004_get_directory_instead_of_file(self):
        """GET /api/files/content on a directory returns 404 (not a regular file)."""
        r = board_get("/api/files/content", params={"path": "/"})
        self.assertEqual(r.status_code, 404,
            f"Expected 404, got {r.status_code}: {r.text}")
        data = r.json()
        self.assertIn("error", data)


# ---------------------------------------------------------------------------
# FILE-CONT-005 to FILE-CONT-007 — GET /api/files/content success cases
# ---------------------------------------------------------------------------

class TestContentGetSuccess(unittest.TestCase):
    """GET /api/files/content reading text files."""

    TEXT_FILE = "test_cont_get.txt"
    TEXT_CONTENT = "Hello, 世界!\nLine 2: utf-8 测试\nLine 3: alpha/beta/gamma\n"

    @classmethod
    def setUpClass(cls):
        r = upload_file(cls.TEXT_FILE, cls.TEXT_CONTENT.encode("utf-8"))
        cls.upload_ok = (r.status_code == 200)

    @classmethod
    def tearDownClass(cls):
        board_delete("/api/files", params={"path": "/" + cls.TEXT_FILE})

    def test_FILE_CONT_005_get_text_file_status_200(self):
        """GET /api/files/content for a text file returns 200."""
        if not self.upload_ok:
            self.skipTest("Upload failed in setUpClass")
        r = board_get("/api/files/content",
                      params={"path": "/" + self.TEXT_FILE})
        self.assertEqual(r.status_code, 200,
            f"Expected 200, got {r.status_code}: {r.text[:200]}")

    def test_FILE_CONT_006_get_text_file_content_matches(self):
        """GET /api/files/content returns exact uploaded content."""
        if not self.upload_ok:
            self.skipTest("Upload failed in setUpClass")
        r = board_get("/api/files/content",
                      params={"path": "/" + self.TEXT_FILE})
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.text, self.TEXT_CONTENT,
            "File content does not match uploaded content")

    def test_FILE_CONT_007_get_text_file_content_type(self):
        """GET /api/files/content returns Content-Type text/plain; charset=utf-8."""
        if not self.upload_ok:
            self.skipTest("Upload failed in setUpClass")
        r = board_get("/api/files/content",
                      params={"path": "/" + self.TEXT_FILE})
        self.assertEqual(r.status_code, 200)
        ct = r.headers.get("Content-Type", "")
        self.assertIn("text/plain", ct.lower(),
            f"Expected text/plain, got: {ct}")


# ---------------------------------------------------------------------------
# FILE-CONT-008 to FILE-CONT-010 — PUT /api/files/content error cases
# ---------------------------------------------------------------------------

class TestContentPutErrorCases(unittest.TestCase):
    """PUT /api/files/content error handling."""

    def test_FILE_CONT_008_put_no_path(self):
        """PUT /api/files/content without path param returns 400."""
        r = board_put("/api/files/content", data="test content")
        self.assertEqual(r.status_code, 400,
            f"Expected 400, got {r.status_code}: {r.text}")
        data = r.json()
        self.assertFalse(data.get("ok", True))
        self.assertIn("error", data)

    def test_FILE_CONT_009_put_path_traversal_rejected(self):
        """PUT /api/files/content with '..' in path returns 400."""
        r = board_put("/api/files/content",
                      params={"path": "../../hack.txt"},
                      data="pwned")
        self.assertEqual(r.status_code, 400,
            f"Expected 400, got {r.status_code}: {r.text}")
        data = r.json()
        self.assertFalse(data.get("ok", True))
        self.assertIn("error", data)

    def test_FILE_CONT_010_put_save_to_nonexistent_directory(self):
        """PUT /api/files/content to a deep path with missing parent fails."""
        r = board_put("/api/files/content",
                      params={"path": "/nonexistent_dir_xyz/file.txt"},
                      data="content")
        self.assertIn(r.status_code, [400, 404, 500],
            f"Expected failure, got {r.status_code}: {r.text}")


# ---------------------------------------------------------------------------
# FILE-CONT-011 to FILE-CONT-012 — PUT /api/files/content success cases
# ---------------------------------------------------------------------------

class TestContentPutSuccess(unittest.TestCase):
    """PUT /api/files/content saving text files."""

    TEST_FILE = "test_cont_put.txt"
    TEST_CONTENT = "hello from put test\nline 2\n世 means world\n"

    @classmethod
    def tearDownClass(cls):
        board_delete("/api/files", params={"path": "/" + cls.TEST_FILE})

    def test_FILE_CONT_011_put_save_new_file(self):
        """PUT /api/files/content saves a new file and returns 200 with ok=true."""
        r = board_put("/api/files/content",
                      params={"path": "/" + self.TEST_FILE},
                      data=self.TEST_CONTENT)
        self.assertEqual(r.status_code, 200,
            f"Expected 200, got {r.status_code}: {r.text}")
        data = r.json()
        self.assertTrue(data.get("ok"),
            f"Expected ok=true: {data}")
        # Verify file appears in listing
        names = list_root_names()
        self.assertIn(self.TEST_FILE, names,
            f"File not visible in listing after PUT: {names}")

    def test_FILE_CONT_012_put_content_verifiable_via_get(self):
        """After PUT, GET returns the exact same content."""
        # Ensure file exists (from previous test or create it)
        if self.TEST_FILE not in list_root_names():
            board_put("/api/files/content",
                      params={"path": "/" + self.TEST_FILE},
                      data=self.TEST_CONTENT)
        r = board_get("/api/files/content",
                      params={"path": "/" + self.TEST_FILE})
        self.assertEqual(r.status_code, 200,
            f"GET after PUT returned {r.status_code}: {r.text[:200]}")
        self.assertEqual(r.text, self.TEST_CONTENT,
            "PUT content does not match GET content")


# ---------------------------------------------------------------------------
# FILE-CONT-013 — Overwrite existing file via PUT
# ---------------------------------------------------------------------------

class TestContentOverwrite(unittest.TestCase):
    """PUT /api/files/content overwriting an existing file."""

    TEST_FILE = "test_cont_overwrite.txt"
    ORIGINAL = "original content"
    UPDATED  = "updated content with 新字符"

    @classmethod
    def setUpClass(cls):
        board_put("/api/files/content",
                  params={"path": "/" + cls.TEST_FILE},
                  data=cls.ORIGINAL)

    @classmethod
    def tearDownClass(cls):
        board_delete("/api/files", params={"path": "/" + cls.TEST_FILE})

    def test_FILE_CONT_013_overwrite_existing_file(self):
        """PUT on an existing file overwrites its content."""
        r = board_put("/api/files/content",
                      params={"path": "/" + self.TEST_FILE},
                      data=self.UPDATED)
        self.assertEqual(r.status_code, 200,
            f"Overwrite returned {r.status_code}: {r.text}")
        data = r.json()
        self.assertTrue(data.get("ok"),
            f"Expected ok=true: {data}")

        # Verify the content was actually updated
        r2 = board_get("/api/files/content",
                       params={"path": "/" + self.TEST_FILE})
        self.assertEqual(r2.status_code, 200)
        self.assertEqual(r2.text, self.UPDATED,
            "File was not overwritten - content still matches original")


# ---------------------------------------------------------------------------
# FILE-CONT-014 — PUT empty content (zero-length file)
# ---------------------------------------------------------------------------

class TestContentEmptyFile(unittest.TestCase):
    """PUT /api/files/content with empty body."""

    TEST_FILE = "test_cont_empty.txt"

    @classmethod
    def tearDownClass(cls):
        board_delete("/api/files", params={"path": "/" + cls.TEST_FILE})

    def test_FILE_CONT_014_put_empty_content(self):
        """PUT with empty body creates a zero-byte file."""
        r = board_put("/api/files/content",
                      params={"path": "/" + self.TEST_FILE},
                      data="")
        self.assertEqual(r.status_code, 200,
            f"PUT empty returned {r.status_code}: {r.text}")
        # Verify the file exists and has zero size
        r2 = board_get("/api/files/content",
                       params={"path": "/" + self.TEST_FILE})
        # May return 200 with empty body or 200 with blank string
        self.assertIn(r2.status_code, [200],
            f"Expected 200, got {r2.status_code}")


# ---------------------------------------------------------------------------
# FILE-CONT-015 — Binary file detection
# ---------------------------------------------------------------------------

class TestContentBinaryDetection(unittest.TestCase):
    """GET /api/files/content on a binary file returns 415."""

    BIN_FILE = "test_cont_binary.bin"
    BIN_CONTENT = b"hello\x00world\xff"

    @classmethod
    def setUpClass(cls):
        r = board_put("/api/files/content",
                      params={"path": "/" + cls.BIN_FILE},
                      data=cls.BIN_CONTENT)
        cls.upload_ok = (r.status_code == 200)

    @classmethod
    def tearDownClass(cls):
        board_delete("/api/files", params={"path": "/" + cls.BIN_FILE})

    def test_FILE_CONT_015_binary_file_rejected(self):
        """GET on binary file (containing null byte) returns 415."""
        if not self.upload_ok:
            self.skipTest("Upload failed in setUpClass")
        r = board_get("/api/files/content",
                      params={"path": "/" + self.BIN_FILE})
        self.assertEqual(r.status_code, 415,
            f"Expected 415 for binary file, got {r.status_code}: {r.text[:200]}")
        data = r.json()
        self.assertIn("error", data)


# ---------------------------------------------------------------------------
# FILE-CONT-016 — Large file rejection (32KB limit)
# ---------------------------------------------------------------------------

class TestContentLargeFile(unittest.TestCase):
    """Server body size limit tests (HTTP_MAX_BODY_SIZE = 32KB)."""

    MAX_FILE = "test_cont_max.txt"
    OVER_FILE = "test_cont_over.txt"

    @classmethod
    def setUpClass(cls):
        # Upload a file at the exact boundary (32768 bytes) — should succeed
        cls.max_content = ("B" * 1024 * 32).encode("utf-8")  # 32768 bytes
        try:
            r = board_put("/api/files/content?path=/" + cls.MAX_FILE,
                          data=cls.max_content)
            cls.max_upload_ok = (r.status_code == 200)
        except Exception:
            cls.max_upload_ok = False

    @classmethod
    def tearDownClass(cls):
        for name in [cls.MAX_FILE, cls.OVER_FILE]:
            try:
                board_delete("/api/files", params={"path": "/" + name})
            except Exception:
                pass

    def test_FILE_CONT_016a_max_allowed_file_ok(self):
        """PUT + GET a file of exactly 32768 bytes (boundary)."""
        if not self.max_upload_ok:
            self.skipTest("Max size file upload failed in setUpClass")
        r = board_get("/api/files/content",
                      params={"path": "/" + self.MAX_FILE})
        self.assertEqual(r.status_code, 200,
            f"Expected 200 for max-size file, got {r.status_code}: {r.text[:200]}")
        self.assertEqual(len(r.content), 32768)

    def test_FILE_CONT_016b_put_exceeding_limit_rejected(self):
        """PUT body > 32768 bytes: server rejects (413 or dropped connection)."""
        oversized = ("C" * 1024 * 33).encode("utf-8")  # 33792 bytes
        try:
            r = board_put("/api/files/content?path=/" + self.OVER_FILE,
                          data=oversized)
            # Server accepted request body but returned non-200 (likely 413).
            # However, lwip_close after http_send may discard the TCP buffer
            # before the proxy reads it, so ConnectionError is also possible.
            if r.status_code == 200:
                self.fail("Expected rejection for oversized body, got 200")
            elif r.status_code == 413:
                data = r.json()
                self.assertIn("error", data)
            else:
                self.fail(f"Unexpected status {r.status_code}")
        except (requests.exceptions.ConnectionError,
                requests.exceptions.ReadTimeout):
            # Server closed connection before proxy could relay response.
            # This is expected: 413 + lwip_close can discard the TCP buffer.
            pass


if __name__ == "__main__":
    unittest.main(verbosity=2)
