"""
CAP 管理 L2 REST API 测试

覆盖范围：

  GET /api/cap/groups — 字段验证
    CAPMGR-001  返回 200，body 为 JSON 数组
    CAPMGR-002  每项含 group_id / plugin_name / runtime_enabled / is_core / llm_visible / tools
    CAPMGR-003  已知组（system / time / files）出现在列表，tools 含已知工具名
    CAPMGR-004  默认（无配置）时非 gateable 组 llm_visible=true / runtime_enabled=true

  POST /api/cap/groups/visibility — LLM 可见性
    CAPMGR-010  {"hidden":[]} 全部可见，200 ok=true
    CAPMGR-011  隐藏单个组后 GET 显示该组 llm_visible=false
    CAPMGR-012  隐藏多个组，各自 llm_visible=false
    CAPMGR-013  持久化到 GET /api/config 的 cap_visibility.hidden
    CAPMGR-014  再次 POST 空列表后该组恢复 llm_visible=true

  POST /api/cap/groups/visibility — 覆盖语义
    CAPMGR-020  第二次 POST 完全替换 hidden 列表（非追加）
    CAPMGR-021  连续多次 POST，最终以最后一次为准

  POST /api/cap/groups/runtime — 运行时启用
    CAPMGR-100  {"disabled":[]} 全部启用，200 ok=true
    CAPMGR-101  禁用单组后 GET 显示 runtime_enabled=false
    CAPMGR-102  被禁用的组仍出现在列表中（区别于 Kconfig 关闭后消失）
    CAPMGR-103  持久化到 GET /api/config 的 cap_runtime.disabled
    CAPMGR-104  再次 POST 空列表后 runtime_enabled 恢复 true
    CAPMGR-105  第二次 POST 完全替换 disabled 列表（非追加）
    CAPMGR-106  CORE 组（lua）is_core=true

  POST /api/cap/groups/runtime — Corner cases
    CAPMGR-120  POST 无 body → 400
    CAPMGR-121  POST 非 JSON body → 400
    CAPMGR-122  POST 缺少 disabled 键 → 400
    CAPMGR-123  POST disabled 是字符串而非数组 → 400
    CAPMGR-124  POST disabled 含非字符串元素 → 跳过，200
    CAPMGR-125  POST disabled 含空字符串 → 空串被跳过，200
    CAPMGR-126  POST disabled 含不存在的 group_id → 200
    CAPMGR-127  POST disabled 恰好 24 个 → 200，全部保存
    CAPMGR-128  POST disabled 超过 24 个 → 200，第 25 个被截断

  cap_list tool（通过 POST /api/cap/invoke）
    CAPMGR-030  cap_list 调用成功，返回含已知工具名的 catalog
    CAPMGR-031  隐藏某组后 cap_list 不再包含该组的工具

  Corner cases（visibility）
    CAPMGR-040~053  （同前）
"""
import unittest
import time
import sys
import os
sys.path.insert(0, os.path.dirname(__file__))
from config import BOARD_BASE_URL, HTTP_TIMEOUT

try:
    import requests
except ImportError:
    raise SystemExit("requests package required: pip install requests")

GROUPS_URL      = BOARD_BASE_URL + "/api/cap/groups"
VISIBILITY_URL  = BOARD_BASE_URL + "/api/cap/groups/visibility"
RUNTIME_URL     = BOARD_BASE_URL + "/api/cap/groups/runtime"
CONFIG_URL      = BOARD_BASE_URL + "/api/config"
INVOKE_URL      = BOARD_BASE_URL + "/api/cap/invoke"

# 安全可隐藏/禁用的组（编译进固件，非 CORE，非 gateable）
SAFE_TO_HIDE        = "web_search"
SAFE_TO_HIDE_2      = "http_request"
SAFE_TO_DISABLE_RT  = "web_search"
SAFE_TO_DISABLE_RT2 = "scheduler"

# 已知 CORE 组（CLAW_CAP_FLAG_CORE，前端不允许禁用）
CORE_GROUP = "lua"

# gateable 组（skill 管理，base visibility 之外）
GATEABLE_GROUPS = {"audio_stream", "board"}


# ── helpers ──────────────────────────────────────────────────────────────────

def _set_hidden(hidden: list):
    return requests.post(VISIBILITY_URL, json={"hidden": hidden}, timeout=HTTP_TIMEOUT)


def _set_runtime_disabled(disabled: list):
    return requests.post(RUNTIME_URL, json={"disabled": disabled}, timeout=HTTP_TIMEOUT)


def _get_groups() -> list:
    r = requests.get(GROUPS_URL, timeout=HTTP_TIMEOUT)
    r.raise_for_status()
    return r.json()


def _find_group(groups: list, group_id: str) -> dict | None:
    for g in groups:
        if g.get("group_id") == group_id:
            return g
    return None


def _reset_visibility():
    try:
        _set_hidden([])
    except Exception:
        pass


def _reset_runtime():
    try:
        _set_runtime_disabled([])
    except Exception:
        pass


def _reset_all():
    _reset_visibility()
    _reset_runtime()


# ── 1. GET /api/cap/groups — 结构与字段 ──────────────────────────────────────

class TestCapGroupsGet(unittest.TestCase):
    """CAPMGR-001~004: GET /api/cap/groups 结构验证."""

    @classmethod
    def setUpClass(cls):
        _reset_all()

    def test_CAPMGR_001_returns_200_json_array(self):
        """CAPMGR-001: GET /api/cap/groups 返回 200，body 是 JSON 数组."""
        r = requests.get(GROUPS_URL, timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200, f"Expected 200, got {r.status_code}: {r.text}")
        self.assertIsInstance(r.json(), list)

    def test_CAPMGR_002_each_item_has_required_fields(self):
        """CAPMGR-002: 每项含 group_id / plugin_name / runtime_enabled / is_core / llm_visible / tools."""
        groups = _get_groups()
        self.assertTrue(len(groups) > 0, "Expected at least one group")
        required = {"group_id", "plugin_name", "runtime_enabled", "is_core", "llm_visible", "tools"}
        for g in groups:
            missing = required - set(g.keys())
            self.assertFalse(missing, f"Group {g.get('group_id')} missing fields: {missing}")
            self.assertIsInstance(g["runtime_enabled"], bool)
            self.assertIsInstance(g["is_core"], bool)
            self.assertIsInstance(g["llm_visible"], bool)
            self.assertIsInstance(g["tools"], list)

    def test_CAPMGR_003_known_groups_present(self):
        """CAPMGR-003: 已知组 system / time / files 出现在列表，tools 含已知工具名."""
        groups = _get_groups()
        ids = {g["group_id"] for g in groups}
        for known in ["system", "time", "files"]:
            self.assertIn(known, ids, f"Expected group '{known}' in list")
        system_g = _find_group(groups, "system")
        self.assertIsNotNone(system_g)
        self.assertIn("get_info", system_g["tools"],
                      f"Expected 'get_info' in system.tools: {system_g['tools']}")

    def test_CAPMGR_004_default_all_enabled_and_visible(self):
        """CAPMGR-004: 无配置时非 gateable 组 llm_visible=true 且 runtime_enabled=true."""
        groups = _get_groups()
        for g in groups:
            if g["group_id"] in GATEABLE_GROUPS:
                continue
            self.assertTrue(g["llm_visible"],
                            f"Group '{g['group_id']}' should be llm_visible=true by default")
            self.assertTrue(g["runtime_enabled"],
                            f"Group '{g['group_id']}' should be runtime_enabled=true by default")


# ── 2. POST — LLM 可见性正常流程 ─────────────────────────────────────────────

class TestCapVisibilitySet(unittest.TestCase):
    """CAPMGR-010~014: POST /api/cap/groups/visibility 正常流程."""

    def tearDown(self):
        _reset_visibility()

    def test_CAPMGR_010_empty_hidden_all_visible(self):
        """CAPMGR-010: {"hidden":[]} 全部可见，返回 200 ok=true."""
        r = _set_hidden([])
        self.assertEqual(r.status_code, 200)
        self.assertTrue(r.json().get("ok"))

    def test_CAPMGR_011_hide_one_group(self):
        """CAPMGR-011: 隐藏单个组后 GET 显示该组 llm_visible=false."""
        _set_hidden([SAFE_TO_HIDE])
        g = _find_group(_get_groups(), SAFE_TO_HIDE)
        self.assertIsNotNone(g)
        self.assertFalse(g["llm_visible"])

    def test_CAPMGR_012_hide_multiple_groups(self):
        """CAPMGR-012: 隐藏多个组，各自 llm_visible=false."""
        _set_hidden([SAFE_TO_HIDE, SAFE_TO_HIDE_2])
        groups = _get_groups()
        for gid in [SAFE_TO_HIDE, SAFE_TO_HIDE_2]:
            g = _find_group(groups, gid)
            self.assertIsNotNone(g)
            self.assertFalse(g["llm_visible"], f"'{gid}' should be llm_visible=false")

    def test_CAPMGR_013_persists_to_config(self):
        """CAPMGR-013: hidden 列表持久化到 GET /api/config 的 cap_visibility.hidden."""
        _set_hidden([SAFE_TO_HIDE])
        cfg = requests.get(CONFIG_URL, timeout=HTTP_TIMEOUT).json()
        hidden = cfg.get("cap_visibility", {}).get("hidden", [])
        self.assertIn(SAFE_TO_HIDE, hidden)

    def test_CAPMGR_014_restore_makes_group_visible_again(self):
        """CAPMGR-014: 先隐藏后 POST 空列表，该组恢复 llm_visible=true."""
        _set_hidden([SAFE_TO_HIDE])
        _set_hidden([])
        g = _find_group(_get_groups(), SAFE_TO_HIDE)
        self.assertIsNotNone(g)
        self.assertTrue(g["llm_visible"])


# ── 3. POST — 覆盖语义 ────────────────────────────────────────────────────────

class TestCapVisibilityOverwrite(unittest.TestCase):
    """CAPMGR-020~021: POST 完全替换，非追加."""

    def setUp(self):
        _reset_visibility()

    def tearDown(self):
        _reset_visibility()

    def test_CAPMGR_020_second_post_replaces_hidden_list(self):
        """CAPMGR-020: 第二次 POST 完全替换 hidden 列表（非追加）."""
        _set_hidden([SAFE_TO_HIDE])
        _set_hidden([SAFE_TO_HIDE_2])
        groups = _get_groups()
        self.assertTrue(_find_group(groups, SAFE_TO_HIDE)["llm_visible"],
                        f"'{SAFE_TO_HIDE}' should be visible after being replaced")
        self.assertFalse(_find_group(groups, SAFE_TO_HIDE_2)["llm_visible"])

    def test_CAPMGR_021_multiple_posts_last_wins(self):
        """CAPMGR-021: 连续多次 POST，最终以最后一次为准."""
        for _ in range(3):
            _set_hidden([SAFE_TO_HIDE])
        _set_hidden([SAFE_TO_HIDE_2])
        cfg = requests.get(CONFIG_URL, timeout=HTTP_TIMEOUT).json()
        hidden = cfg.get("cap_visibility", {}).get("hidden", [])
        self.assertNotIn(SAFE_TO_HIDE, hidden)
        self.assertIn(SAFE_TO_HIDE_2, hidden)


# ── 4. POST /api/cap/groups/runtime — 运行时启用 ─────────────────────────────

class TestCapRuntimeSet(unittest.TestCase):
    """CAPMGR-100~106: POST /api/cap/groups/runtime 正常流程."""

    def setUp(self):
        _reset_all()

    def tearDown(self):
        _reset_all()

    def test_CAPMGR_100_empty_disabled_all_enabled(self):
        """CAPMGR-100: {"disabled":[]} 全部启用，返回 200 ok=true."""
        r = _set_runtime_disabled([])
        self.assertEqual(r.status_code, 200)
        self.assertTrue(r.json().get("ok"))

    def test_CAPMGR_101_disable_one_group_shows_false(self):
        """CAPMGR-101: 禁用单组后 GET 显示该组 runtime_enabled=false."""
        r = _set_runtime_disabled([SAFE_TO_DISABLE_RT])
        self.assertEqual(r.status_code, 200)
        self.assertTrue(r.json().get("ok"))
        g = _find_group(_get_groups(), SAFE_TO_DISABLE_RT)
        self.assertIsNotNone(g, f"Group '{SAFE_TO_DISABLE_RT}' not found")
        self.assertFalse(g["runtime_enabled"],
                         f"'{SAFE_TO_DISABLE_RT}' should be runtime_enabled=false")

    def test_CAPMGR_102_disabled_group_still_in_list(self):
        """CAPMGR-102: 运行时禁用的组仍出现在列表中（区别于 Kconfig 关闭后消失）."""
        _set_runtime_disabled([SAFE_TO_DISABLE_RT])
        groups = _get_groups()
        ids = {g["group_id"] for g in groups}
        self.assertIn(SAFE_TO_DISABLE_RT, ids,
                      f"Runtime-disabled group '{SAFE_TO_DISABLE_RT}' should still appear in list. "
                      f"Got: {ids}")

    def test_CAPMGR_103_persists_to_config(self):
        """CAPMGR-103: disabled 列表持久化到 GET /api/config 的 cap_runtime.disabled."""
        _set_runtime_disabled([SAFE_TO_DISABLE_RT])
        cfg = requests.get(CONFIG_URL, timeout=HTTP_TIMEOUT).json()
        self.assertIn("cap_runtime", cfg, "GET /api/config should contain cap_runtime section")
        disabled = cfg["cap_runtime"].get("disabled", [])
        self.assertIn(SAFE_TO_DISABLE_RT, disabled,
                      f"'{SAFE_TO_DISABLE_RT}' should be in cap_runtime.disabled: {disabled}")

    def test_CAPMGR_104_restore_makes_group_enabled_again(self):
        """CAPMGR-104: 先禁用后 POST 空列表，runtime_enabled 恢复 true."""
        _set_runtime_disabled([SAFE_TO_DISABLE_RT])
        _set_runtime_disabled([])
        g = _find_group(_get_groups(), SAFE_TO_DISABLE_RT)
        self.assertIsNotNone(g)
        self.assertTrue(g["runtime_enabled"],
                        f"'{SAFE_TO_DISABLE_RT}' should be runtime_enabled=true after clearing")

    def test_CAPMGR_105_second_post_replaces_disabled_list(self):
        """CAPMGR-105: 第二次 POST 完全替换 disabled 列表（非追加）."""
        _set_runtime_disabled([SAFE_TO_DISABLE_RT])
        _set_runtime_disabled([SAFE_TO_DISABLE_RT2])
        groups = _get_groups()
        g1 = _find_group(groups, SAFE_TO_DISABLE_RT)
        g2 = _find_group(groups, SAFE_TO_DISABLE_RT2)
        self.assertIsNotNone(g1)
        self.assertIsNotNone(g2)
        self.assertTrue(g1["runtime_enabled"],
                        f"'{SAFE_TO_DISABLE_RT}' should be re-enabled after replace")
        self.assertFalse(g2["runtime_enabled"],
                         f"'{SAFE_TO_DISABLE_RT2}' should be disabled by second POST")

    def test_CAPMGR_106_core_group_is_core_true(self):
        """CAPMGR-106: CORE 组（lua）is_core=true."""
        groups = _get_groups()
        g = _find_group(groups, CORE_GROUP)
        if g is None:
            self.skipTest(f"CORE group '{CORE_GROUP}' not found in list (may be absent in this build)")
        self.assertTrue(g["is_core"],
                        f"Group '{CORE_GROUP}' should have is_core=true, got: {g}")


# ── 5. POST /api/cap/groups/runtime — Corner cases ───────────────────────────

class TestCapRuntimeCornerCases(unittest.TestCase):
    """CAPMGR-120~128: runtime 边界与异常输入."""

    def setUp(self):
        _reset_all()

    def tearDown(self):
        _reset_all()

    def test_CAPMGR_120_empty_body_rejected(self):
        """CAPMGR-120: POST 无 body → 400."""
        r = requests.post(RUNTIME_URL, data=b"", timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 400)

    def test_CAPMGR_121_invalid_json_rejected(self):
        """CAPMGR-121: POST 非 JSON body → 400."""
        r = requests.post(RUNTIME_URL, data=b"not-json",
                          headers={"Content-Type": "application/json"},
                          timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 400)

    def test_CAPMGR_122_missing_disabled_key_rejected(self):
        """CAPMGR-122: POST 缺少 disabled 键 → 400."""
        r = requests.post(RUNTIME_URL, json={"groups": []}, timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 400)

    def test_CAPMGR_123_disabled_not_array_rejected(self):
        """CAPMGR-123: POST disabled 是字符串而非数组 → 400."""
        r = requests.post(RUNTIME_URL, json={"disabled": "web_search"}, timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 400)

    def test_CAPMGR_124_non_string_elements_skipped(self):
        """CAPMGR-124: disabled 含非字符串元素 → 跳过非字符串，200 成功."""
        r = requests.post(RUNTIME_URL,
                          json={"disabled": [123, SAFE_TO_DISABLE_RT, None, True]},
                          timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200)
        self.assertTrue(r.json().get("ok"))
        cfg = requests.get(CONFIG_URL, timeout=HTTP_TIMEOUT).json()
        disabled = cfg.get("cap_runtime", {}).get("disabled", [])
        self.assertIn(SAFE_TO_DISABLE_RT, disabled,
                      f"Valid string should be stored: {disabled}")

    def test_CAPMGR_125_empty_string_skipped(self):
        """CAPMGR-125: disabled 含空字符串 → 空串被跳过，200 成功."""
        r = requests.post(RUNTIME_URL,
                          json={"disabled": ["", SAFE_TO_DISABLE_RT, ""]},
                          timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200)
        cfg = requests.get(CONFIG_URL, timeout=HTTP_TIMEOUT).json()
        disabled = cfg.get("cap_runtime", {}).get("disabled", [])
        self.assertNotIn("", disabled)
        self.assertIn(SAFE_TO_DISABLE_RT, disabled)

    def test_CAPMGR_126_nonexistent_group_accepted(self):
        """CAPMGR-126: disabled 含不存在的 group_id → 200（保存成功，不报错）."""
        r = requests.post(RUNTIME_URL,
                          json={"disabled": ["phantom_group_xyz"]},
                          timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200)
        self.assertTrue(r.json().get("ok"))
        # 真实 cap 不受影响
        g = _find_group(_get_groups(), "system")
        self.assertIsNotNone(g)
        self.assertTrue(g["runtime_enabled"])

    def test_CAPMGR_127_exactly_max_24_groups(self):
        """CAPMGR-127: disabled 恰好 24 个 → 200，全部保存."""
        groups_24 = [f"phantom_{i:02d}" for i in range(24)]
        r = requests.post(RUNTIME_URL, json={"disabled": groups_24}, timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200)
        self.assertTrue(r.json().get("ok"))
        cfg = requests.get(CONFIG_URL, timeout=HTTP_TIMEOUT).json()
        disabled = cfg.get("cap_runtime", {}).get("disabled", [])
        self.assertEqual(len(disabled), 24, f"Expected 24 entries, got {len(disabled)}")

    def test_CAPMGR_128_over_max_truncated(self):
        """CAPMGR-128: disabled 超过 24 个 → 200，第 25 个被截断."""
        groups_25 = [f"phantom_{i:02d}" for i in range(25)]
        r = requests.post(RUNTIME_URL, json={"disabled": groups_25}, timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200)
        cfg = requests.get(CONFIG_URL, timeout=HTTP_TIMEOUT).json()
        disabled = cfg.get("cap_runtime", {}).get("disabled", [])
        self.assertLessEqual(len(disabled), 24)
        self.assertNotIn("phantom_24", disabled, "25th entry should be truncated")


# ── 6. cap_list tool ──────────────────────────────────────────────────────────

class TestCapListTool(unittest.TestCase):
    """CAPMGR-030~031: cap_list 通过 /api/cap/invoke 测试."""

    def setUp(self):
        _reset_all()

    def tearDown(self):
        _reset_all()

    def _invoke_cap_list(self):
        return requests.post(INVOKE_URL, json={"cap": "cap_list", "input": {}},
                             timeout=HTTP_TIMEOUT)

    def test_CAPMGR_030_cap_list_returns_catalog(self):
        """CAPMGR-030: cap_list 调用成功，返回含已知工具名的 catalog."""
        r = self._invoke_cap_list()
        self.assertEqual(r.status_code, 200)
        body = r.text
        self.assertTrue(len(body) > 10, f"cap_list returned empty: {body!r}")
        for known in ["get_heap_info", "get_info", "cap_list"]:
            self.assertIn(known, body, f"Expected '{known}' in catalog")
        self.assertIn("net_discover_stop", body,
                      f"Expected net_discover_stop in catalog: {body[:400]}")

    def test_CAPMGR_031_hidden_group_absent_from_cap_list(self):
        """CAPMGR-031: 隐藏某组后 cap_list 不再包含该组的工具（net_discover）."""
        r_before = self._invoke_cap_list()
        self.assertIn("net_discover_stop", r_before.text)

        _set_hidden(["net_discover"])
        r_after = self._invoke_cap_list()
        self.assertEqual(r_after.status_code, 200)
        self.assertNotIn("net_discover_stop", r_after.text,
                         "net_discover_stop should not appear after hiding group")


# ── 7. Corner cases（visibility）────────────────────────────────────────────

class TestCapVisibilityCornerCases(unittest.TestCase):
    """CAPMGR-040~053: visibility 边界与异常输入."""

    def setUp(self):
        _reset_all()

    def tearDown(self):
        _reset_all()

    def test_CAPMGR_040_empty_body_rejected(self):
        """CAPMGR-040: POST 无 body → 400."""
        r = requests.post(VISIBILITY_URL, data=b"", timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 400)

    def test_CAPMGR_041_invalid_json_rejected(self):
        """CAPMGR-041: POST 非 JSON body → 400."""
        r = requests.post(VISIBILITY_URL, data=b"not-json",
                          headers={"Content-Type": "application/json"},
                          timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 400)

    def test_CAPMGR_042_missing_hidden_key_rejected(self):
        """CAPMGR-042: POST 缺少 hidden 键 → 400."""
        r = requests.post(VISIBILITY_URL, json={"groups": []}, timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 400)

    def test_CAPMGR_043_hidden_not_array_rejected(self):
        """CAPMGR-043: POST hidden 是字符串而非数组 → 400."""
        r = requests.post(VISIBILITY_URL, json={"hidden": "web_search"}, timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 400)

    def test_CAPMGR_044_non_string_elements_skipped(self):
        """CAPMGR-044: hidden 含非字符串元素 → 跳过，200 成功."""
        r = requests.post(VISIBILITY_URL,
                          json={"hidden": [123, SAFE_TO_HIDE, None, True]},
                          timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200)
        self.assertTrue(r.json().get("ok"))
        cfg = requests.get(CONFIG_URL, timeout=HTTP_TIMEOUT).json()
        hidden = cfg.get("cap_visibility", {}).get("hidden", [])
        self.assertIn(SAFE_TO_HIDE, hidden)

    def test_CAPMGR_045_empty_string_skipped(self):
        """CAPMGR-045: hidden 含空字符串 → 空串被跳过，200 成功."""
        r = requests.post(VISIBILITY_URL,
                          json={"hidden": ["", SAFE_TO_HIDE, ""]},
                          timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200)
        cfg = requests.get(CONFIG_URL, timeout=HTTP_TIMEOUT).json()
        hidden = cfg.get("cap_visibility", {}).get("hidden", [])
        self.assertNotIn("", hidden)
        self.assertIn(SAFE_TO_HIDE, hidden)

    def test_CAPMGR_046_nonexistent_group_accepted(self):
        """CAPMGR-046: hidden 含不存在的 group_id → 200。"""
        r = requests.post(VISIBILITY_URL, json={"hidden": ["phantom_xyz"]}, timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200)
        self.assertTrue(r.json().get("ok"))
        g = _find_group(_get_groups(), "system")
        self.assertIsNotNone(g)
        self.assertTrue(g["llm_visible"])

    def test_CAPMGR_047_duplicate_group_ids_accepted(self):
        """CAPMGR-047: hidden 含重复 group_id → 200。"""
        r = requests.post(VISIBILITY_URL,
                          json={"hidden": [SAFE_TO_HIDE, SAFE_TO_HIDE, SAFE_TO_HIDE]},
                          timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200)
        g = _find_group(_get_groups(), SAFE_TO_HIDE)
        self.assertFalse(g["llm_visible"])

    def test_CAPMGR_048_exactly_max_24_groups(self):
        """CAPMGR-048: hidden 恰好 24 个 → 200，全部保存."""
        groups_24 = [f"phantom_{i:02d}" for i in range(24)]
        r = requests.post(VISIBILITY_URL, json={"hidden": groups_24}, timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200)
        cfg = requests.get(CONFIG_URL, timeout=HTTP_TIMEOUT).json()
        hidden = cfg.get("cap_visibility", {}).get("hidden", [])
        self.assertEqual(len(hidden), 24)

    def test_CAPMGR_049_over_max_truncated(self):
        """CAPMGR-049: hidden 超过 24 个 → 200，只保存前 24 个。"""
        groups_25 = [f"phantom_{i:02d}" for i in range(25)]
        r = requests.post(VISIBILITY_URL, json={"hidden": groups_25}, timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200)
        cfg = requests.get(CONFIG_URL, timeout=HTTP_TIMEOUT).json()
        hidden = cfg.get("cap_visibility", {}).get("hidden", [])
        self.assertLessEqual(len(hidden), 24)
        self.assertNotIn("phantom_24", hidden)

    def test_CAPMGR_053_hide_all_real_groups_cap_list_empty(self):
        """CAPMGR-053: 隐藏全部真实 group 后 cap_list 返回空工具列表（sentinel 回归）."""
        all_groups = [g["group_id"] for g in _get_groups()]
        r = requests.post(VISIBILITY_URL, json={"hidden": all_groups}, timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200)
        time.sleep(0.5)
        r_list = requests.post(INVOKE_URL, json={"cap": "cap_list", "input": {}},
                               timeout=HTTP_TIMEOUT)
        self.assertEqual(r_list.status_code, 200)
        self.assertNotIn("[", r_list.text,
                         f"Expected empty catalog after hiding all groups: {r_list.text[:300]}")

    def test_CAPMGR_050_gateable_groups_unaffected(self):
        """CAPMGR-050: gateable 组（board/audio_stream）可被标记但系统不崩溃."""
        _set_hidden(list(GATEABLE_GROUPS))
        groups = _get_groups()
        for gid in GATEABLE_GROUPS:
            g = _find_group(groups, gid)
            if g is None:
                continue
            self.assertIn("llm_visible", g)

    def test_CAPMGR_051_group_id_63_bytes_accepted(self):
        """CAPMGR-051: group_id 恰好 63 字节 → 保存后读回正确."""
        gid_63 = "x" * 63
        r = requests.post(VISIBILITY_URL, json={"hidden": [gid_63]}, timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200)
        cfg = requests.get(CONFIG_URL, timeout=HTTP_TIMEOUT).json()
        hidden = cfg.get("cap_visibility", {}).get("hidden", [])
        self.assertIn(gid_63, hidden)

    def test_CAPMGR_052_group_id_64_bytes_truncated(self):
        """CAPMGR-052: group_id 恰好 64 字节 → strlcpy 截断为 63 字节，不崩溃."""
        gid_64 = "y" * 64
        r = requests.post(VISIBILITY_URL, json={"hidden": [gid_64]}, timeout=HTTP_TIMEOUT)
        self.assertIn(r.status_code, [200, 400])
        # 系统仍可响应
        self.assertEqual(requests.get(GROUPS_URL, timeout=HTTP_TIMEOUT).status_code, 200)


if __name__ == "__main__":
    unittest.main(verbosity=2)
