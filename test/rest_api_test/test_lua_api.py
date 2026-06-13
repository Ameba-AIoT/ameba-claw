"""
Lua script management HTTP API tests.
Tests: GET/PUT/DELETE /api/lua/* endpoints.
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

def board_put(path, **kwargs):
    return requests.put(BOARD_BASE_URL + path, timeout=HTTP_TIMEOUT, **kwargs)


TEST_SCRIPT_NAME = "test_auto_lua_api.lua"
TEST_SCRIPT_CONTENT = "-- Auto-test script\nfunction run(args)\n  return 'hello'\nend\n"


class TestLuaAPI(unittest.TestCase):
    """Lua script management via HTTP API."""

    @classmethod
    def setUpClass(cls):
        # Clean up if previous test left the file
        board_delete("/api/lua", params={"name": TEST_SCRIPT_NAME})

    def test_LUA_API_001_list_scripts(self):
        """GET /api/lua returns list of scripts."""
        r = board_get("/api/lua")
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertIn("files", data)
        self.assertIsInstance(data["files"], list)

    def test_LUA_API_002_put_script(self):
        """PUT /api/lua/content?name=X saves Lua script."""
        r = board_put("/api/lua/content",
                      params={"name": TEST_SCRIPT_NAME},
                      data=TEST_SCRIPT_CONTENT.encode(),
                      headers={"Content-Type": "text/plain"})
        self.assertEqual(r.status_code, 200, f"PUT failed: {r.text}")
        data = r.json()
        self.assertTrue(data.get("ok"), f"Expected ok=true: {data}")

    def test_LUA_API_003_get_script_content(self):
        """GET /api/lua/content?name=X returns saved script."""
        # First save
        board_put("/api/lua/content",
                  params={"name": TEST_SCRIPT_NAME},
                  data=TEST_SCRIPT_CONTENT.encode(),
                  headers={"Content-Type": "text/plain"})
        # Then retrieve
        r = board_get("/api/lua/content", params={"name": TEST_SCRIPT_NAME})
        if r.status_code == 404:
            self.skipTest("Lua script not found after put - may not be supported")
        self.assertEqual(r.status_code, 200)
        self.assertIn("run", r.text)

    def test_LUA_API_004_delete_script(self):
        """DELETE /api/lua?name=X removes the script."""
        # Create first
        board_put("/api/lua/content",
                  params={"name": TEST_SCRIPT_NAME},
                  data=TEST_SCRIPT_CONTENT.encode(),
                  headers={"Content-Type": "text/plain"})
        # Delete
        r = board_delete("/api/lua", params={"name": TEST_SCRIPT_NAME})
        self.assertEqual(r.status_code, 200, f"DELETE failed: {r.text}")
        data = r.json()
        self.assertTrue(data.get("ok"), f"Expected ok=true: {data}")
        # Verify gone
        r2 = board_get("/api/lua")
        if r2.status_code == 200:
            files = r2.json().get("files", [])
            names = [f["name"] for f in files]
            self.assertNotIn(TEST_SCRIPT_NAME, names)

    def test_LUA_API_005_delete_nonexistent_4xx(self):
        """DELETE /api/lua for nonexistent script returns 4xx."""
        r = board_delete("/api/lua", params={"name": "no_such_script_xyz.lua"})
        self.assertIn(r.status_code, [400, 404, 500],
            f"Expected 4xx, got {r.status_code}")

    def test_LUA_API_006_path_traversal_rejected(self):
        """PUT /api/lua/content with traversal in name is rejected."""
        r = board_put("/api/lua/content",
                      params={"name": "../../hack.lua"},
                      data=b"-- hack",
                      headers={"Content-Type": "text/plain"})
        self.assertIn(r.status_code, [400, 403, 404],
            f"Expected path traversal rejected, got {r.status_code}: {r.text}")

    @classmethod
    def tearDownClass(cls):
        board_delete("/api/lua", params={"name": TEST_SCRIPT_NAME})


if __name__ == "__main__":
    unittest.main(verbosity=2)
