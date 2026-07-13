"""
HTTP 请求配置 API 测试 (cap_http_request / allowlist)

覆盖范围：
  HTTPCFG-001  GET /api/config 包含 http_request.allowlist 字段
  HTTPCFG-002  POST /setup section=http_request 保存 * (放行所有)
  HTTPCFG-003  保存多行有效规则
  HTTPCFG-004  保存空字符串 (拒绝所有模式)
  HTTPCFG-005  保存通配符子域名规则
  HTTPCFG-006  保存含端口号规则
  HTTPCFG-007  保存含空行规则 (空行应被忽略，不触发校验错误)
  HTTPCFG-010  非法规则：含空格 → 400
  HTTPCFG-011  非法规则：含斜杠 (http://...) → 400
  HTTPCFG-012  非法规则：含 @ 符号 → 400
  HTTPCFG-013  非法规则：含感叹号 → 400
  HTTPCFG-014  缺少 section 字段 → 400
  HTTPCFG-015  非 JSON body → 400
  HTTPCFG-016  错误信息包含具体规则内容
  HTTPCFG-020  保存后立即读回一致 (in-session 持久化)
  HTTPCFG-021  多次写入，最终读回为最后一次值
  HTTPCFG-058  规则含端口 ip-api.com:80 应能匹配请求（Bug#2 回归）
  HTTPCFG-059  规则端口号不限制实际端口，匹配按主机名
  HTTPCFG-060  规则含端口但主机名不匹配时仍拦截
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

SETUP_URL  = BOARD_BASE_URL + "/setup"
CONFIG_URL = BOARD_BASE_URL + "/api/config"
RESTART_URL = BOARD_BASE_URL + "/api/system/restart"


def _save_allowlist(allowlist: str):
    """POST /setup with http_request section."""
    return requests.post(
        SETUP_URL,
        json={"section": "http_request", "allowlist": allowlist},
        timeout=HTTP_TIMEOUT,
    )


def _get_allowlist() -> str:
    """GET /api/config and return http_request.allowlist value."""
    r = requests.get(CONFIG_URL, timeout=HTTP_TIMEOUT)
    r.raise_for_status()
    return r.json().get("http_request", {}).get("allowlist", None)


class TestHttpRequestConfigRead(unittest.TestCase):
    """HTTPCFG-001: GET /api/config 包含 http_request 段."""

    def test_HTTPCFG_001_config_has_http_request_section(self):
        """GET /api/config 返回 http_request.allowlist 字段."""
        r = requests.get(CONFIG_URL, timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 200, f"GET /api/config failed: {r.status_code}")
        data = r.json()
        self.assertIn("http_request", data,
                      "Response missing 'http_request' section")
        self.assertIn("allowlist", data["http_request"],
                      "http_request section missing 'allowlist' field")
        self.assertIsInstance(data["http_request"]["allowlist"], str,
                              "allowlist should be a string")


class TestHttpRequestConfigSave(unittest.TestCase):
    """HTTPCFG-002~007: 合法 allowlist 保存测试."""

    @classmethod
    def setUpClass(cls):
        cls._original = _get_allowlist()

    @classmethod
    def tearDownClass(cls):
        if cls._original is not None:
            try:
                _save_allowlist(cls._original)
            except Exception:
                pass

    def test_HTTPCFG_002_save_star_allow_all(self):
        """HTTPCFG-002: allowlist='*' 返回 200 ok=true."""
        r = _save_allowlist("*")
        self.assertEqual(r.status_code, 200, f"Expected 200, got {r.status_code}: {r.text}")
        self.assertTrue(r.json().get("ok"), f"Expected ok=true: {r.json()}")

    def test_HTTPCFG_003_save_multi_rule(self):
        """HTTPCFG-003: 多行有效规则保存成功."""
        rules = "ip-api.com\napi.bilibili.com\n*.example.com"
        r = _save_allowlist(rules)
        self.assertEqual(r.status_code, 200, f"Expected 200, got {r.status_code}: {r.text}")
        self.assertTrue(r.json().get("ok"), f"Expected ok=true: {r.json()}")

    def test_HTTPCFG_004_save_empty_deny_all(self):
        """HTTPCFG-004: allowlist='' (拒绝所有模式) 保存成功."""
        r = _save_allowlist("")
        self.assertEqual(r.status_code, 200, f"Expected 200, got {r.status_code}: {r.text}")
        self.assertTrue(r.json().get("ok"), f"Expected ok=true: {r.json()}")

    def test_HTTPCFG_005_save_wildcard_subdomain(self):
        """HTTPCFG-005: 通配符子域名规则 *.api.example.com 保存成功."""
        r = _save_allowlist("*.api.example.com")
        self.assertEqual(r.status_code, 200, f"Expected 200, got {r.status_code}: {r.text}")
        self.assertTrue(r.json().get("ok"), f"Expected ok=true: {r.json()}")

    def test_HTTPCFG_006_save_rule_with_port(self):
        """HTTPCFG-006: 含端口号规则 api.example.com:8443 保存成功."""
        r = _save_allowlist("api.example.com:8443")
        self.assertEqual(r.status_code, 200, f"Expected 200, got {r.status_code}: {r.text}")
        self.assertTrue(r.json().get("ok"), f"Expected ok=true: {r.json()}")

    def test_HTTPCFG_007_save_rules_with_blank_lines(self):
        """HTTPCFG-007: 含空行的规则列表，空行应被忽略，整体保存成功."""
        rules = "ip-api.com\n\n\napi.bilibili.com\n"
        r = _save_allowlist(rules)
        self.assertEqual(r.status_code, 200, f"Expected 200, got {r.status_code}: {r.text}")
        self.assertTrue(r.json().get("ok"), f"Expected ok=true: {r.json()}")


class TestHttpRequestAllowlistValidation(unittest.TestCase):
    """HTTPCFG-010~016: 非法 allowlist 格式检查."""

    @classmethod
    def setUpClass(cls):
        cls._original = _get_allowlist()

    @classmethod
    def tearDownClass(cls):
        if cls._original is not None:
            try:
                _save_allowlist(cls._original)
            except Exception:
                pass

    def _assert_rejected(self, allowlist: str, msg: str = ""):
        """Helper: expect 400 with ok=false."""
        r = _save_allowlist(allowlist)
        self.assertEqual(r.status_code, 400,
                         f"{msg} — expected 400, got {r.status_code}: {r.text}")
        data = r.json()
        self.assertFalse(data.get("ok", True),
                         f"{msg} — expected ok=false: {data}")

    def test_HTTPCFG_010_invalid_rule_with_space(self):
        """HTTPCFG-010: 规则含空格 → 400."""
        self._assert_rejected("bad rule", "rule with space")

    def test_HTTPCFG_011_invalid_rule_with_slash(self):
        """HTTPCFG-011: 规则含斜杠 (http://host) → 400."""
        self._assert_rejected("http://example.com", "rule with slash")

    def test_HTTPCFG_012_invalid_rule_with_at(self):
        """HTTPCFG-012: 规则含 @ 符号 → 400."""
        self._assert_rejected("user@example.com", "rule with @")

    def test_HTTPCFG_013_invalid_rule_with_exclamation(self):
        """HTTPCFG-013: 规则含感叹号 → 400."""
        self._assert_rejected("example.com!", "rule with !")

    def test_HTTPCFG_014_invalid_rule_mixed_valid_invalid(self):
        """HTTPCFG-014: 多行规则中含一条非法规则 → 400 (整体拒绝)."""
        rules = "ip-api.com\nbad rule!\napi.bilibili.com"
        self._assert_rejected(rules, "mixed valid+invalid rules")

    def test_HTTPCFG_015_error_message_contains_bad_rule(self):
        """HTTPCFG-015: 400 错误消息应包含具体的非法规则内容."""
        bad = "has space"
        r = _save_allowlist(bad)
        self.assertEqual(r.status_code, 400,
                         f"Expected 400, got {r.status_code}: {r.text}")
        body_text = r.text
        self.assertIn(bad, body_text,
                      f"Error response should mention the bad rule '{bad}', got: {body_text}")

    def test_HTTPCFG_016_missing_section_field(self):
        """HTTPCFG-016: POST /setup 缺少 section 字段 → 400."""
        r = requests.post(SETUP_URL,
                          json={"allowlist": "*"},
                          timeout=HTTP_TIMEOUT)
        self.assertEqual(r.status_code, 400,
                         f"Expected 400, got {r.status_code}: {r.text}")

    def test_HTTPCFG_017_non_json_body(self):
        """HTTPCFG-017: POST /setup 非 JSON body → 400."""
        r = requests.post(SETUP_URL,
                          data="section=http_request&allowlist=*",
                          headers={"Content-Type": "application/x-www-form-urlencoded"},
                          timeout=HTTP_TIMEOUT)
        self.assertIn(r.status_code, [400, 500],
                      f"Expected 400/500 for non-JSON, got {r.status_code}: {r.text}")


class TestHttpRequestConfigPersistence(unittest.TestCase):
    """HTTPCFG-020~022: allowlist 持久化测试."""

    @classmethod
    def setUpClass(cls):
        cls._original = _get_allowlist()

    @classmethod
    def tearDownClass(cls):
        if cls._original is not None:
            try:
                _save_allowlist(cls._original)
            except Exception:
                pass

    def test_HTTPCFG_020_save_readback_consistent(self):
        """HTTPCFG-020: 保存后立即读回，值与写入一致 (in-session 持久化)."""
        value = "ip-api.com\napi.github.com"
        r = _save_allowlist(value)
        self.assertEqual(r.status_code, 200, f"Save failed: {r.text}")
        self.assertTrue(r.json().get("ok"))

        readback = _get_allowlist()
        self.assertIsNotNone(readback, "Could not read back allowlist")
        self.assertEqual(readback, value,
                         f"Readback mismatch:\n  wrote:    {repr(value)}\n  readback: {repr(readback)}")

    def test_HTTPCFG_021_overwrite_readback_last_value(self):
        """HTTPCFG-021: 多次写入，GET /api/config 返回最后一次保存的值."""
        _save_allowlist("first.example.com")
        time.sleep(0.1)
        second = "second.example.com\n*.cdn.net"
        _save_allowlist(second)
        time.sleep(0.1)

        readback = _get_allowlist()
        self.assertEqual(readback, second,
                         f"Expected last-written value, got: {repr(readback)}")

    def test_HTTPCFG_022_save_star_readback(self):
        """HTTPCFG-022: 保存 '*' 后读回仍为 '*'."""
        _save_allowlist("*")
        readback = _get_allowlist()
        self.assertEqual(readback, "*",
                         f"Expected '*', got: {repr(readback)}")

    def test_HTTPCFG_023_save_empty_readback(self):
        """HTTPCFG-023: 保存空字符串后读回仍为空 (deny-all 状态持久化)."""
        _save_allowlist("")
        readback = _get_allowlist()
        self.assertEqual(readback, "",
                         f"Expected empty string, got: {repr(readback)}")

class TestHttpRequestAllowlistBoundary(unittest.TestCase):
    """
    HTTPCFG-030~037: allowlist 长度边界检查.

    覆盖新增的两项服务端校验：
      - 单条规则超过 253 字符 → 400
      - allowlist 总长度 ≥ 512 字节 → 400
    以及 wildcard_match 多星号支持。
    """

    @classmethod
    def setUpClass(cls):
        cls._original = _get_allowlist()

    @classmethod
    def tearDownClass(cls):
        if cls._original is not None:
            try:
                _save_allowlist(cls._original)
            except Exception:
                pass

    def test_HTTPCFG_030_rule_253_chars_accepted(self):
        """HTTPCFG-030: 单条规则恰好 253 字符（DNS 最大主机名）→ 200."""
        rule = "a" * 253
        self.assertEqual(len(rule), 253)
        r = _save_allowlist(rule)
        self.assertEqual(r.status_code, 200,
                         f"Expected 200 for 253-char rule, got {r.status_code}: {r.text}")
        self.assertTrue(r.json().get("ok"), f"Expected ok=true: {r.json()}")

    def test_HTTPCFG_031_rule_254_chars_rejected(self):
        """HTTPCFG-031: 单条规则 254 字符（超出 DNS 限制）→ 400."""
        rule = "a" * 254
        self.assertEqual(len(rule), 254)
        r = _save_allowlist(rule)
        self.assertEqual(r.status_code, 400,
                         f"Expected 400 for 254-char rule, got {r.status_code}: {r.text}")
        self.assertFalse(r.json().get("ok", True), f"Expected ok=false: {r.json()}")

    def test_HTTPCFG_032_too_long_rule_error_message(self):
        """HTTPCFG-032: 超长规则错误消息包含 'too long' 且含规则前缀."""
        rule = "b" * 254
        r = _save_allowlist(rule)
        self.assertEqual(r.status_code, 400)
        body_text = r.text.lower()
        self.assertIn("too long", body_text,
                      f"Error should say 'too long', got: {r.text}")
        # 错误消息应包含规则的前 60 字符（trunc[61] 截断）
        self.assertIn("b" * 60, r.text,
                      f"Error should contain first 60 chars of the rule, got: {r.text}")

    def test_HTTPCFG_033_total_length_511_accepted(self):
        """HTTPCFG-033: allowlist 总长度 511 字符（最大合法值）→ 200."""
        # 253 + '\n' + 253 + '\n' + 'ccc' = 511，每条规则 ≤ 253
        allowlist = "a" * 253 + "\n" + "b" * 253 + "\n" + "ccc"
        self.assertEqual(len(allowlist), 511)
        r = _save_allowlist(allowlist)
        self.assertEqual(r.status_code, 200,
                         f"Expected 200 for 511-char allowlist, got {r.status_code}: {r.text}")
        self.assertTrue(r.json().get("ok"), f"Expected ok=true: {r.json()}")

    def test_HTTPCFG_034_total_length_512_rejected(self):
        """HTTPCFG-034: allowlist 总长度 512 字符（超出存储上限）→ 400."""
        # 253 + '\n' + 253 + '\n' + 'cccc' = 512，总长超出限制
        allowlist = "a" * 253 + "\n" + "b" * 253 + "\n" + "cccc"
        self.assertEqual(len(allowlist), 512)
        r = _save_allowlist(allowlist)
        self.assertEqual(r.status_code, 400,
                         f"Expected 400 for 512-char allowlist, got {r.status_code}: {r.text}")

    def test_HTTPCFG_035_second_rule_too_long_rejected(self):
        """HTTPCFG-035: 多行规则中第二条超过 253 字符 → 400（整体拒绝）."""
        allowlist = "valid.example.com\n" + "a" * 254
        r = _save_allowlist(allowlist)
        self.assertEqual(r.status_code, 400,
                         f"Expected 400, got {r.status_code}: {r.text}")
        self.assertFalse(r.json().get("ok", True), f"Expected ok=false: {r.json()}")

    def test_HTTPCFG_036_multi_star_wildcard_accepted(self):
        """HTTPCFG-036: 多星号通配符规则（如 *.s3.*.amazonaws.com）→ 200."""
        rule = "*.s3.*.amazonaws.com"
        r = _save_allowlist(rule)
        self.assertEqual(r.status_code, 200,
                         f"Expected 200 for multi-star wildcard, got {r.status_code}: {r.text}")
        self.assertTrue(r.json().get("ok"), f"Expected ok=true: {r.json()}")

    def test_HTTPCFG_037_total_length_exactly_one_under_limit(self):
        """HTTPCFG-037: allowlist 总长度 510 字符（明显低于上限）→ 200."""
        # 253 + '\n' + 253 + '\n' + 'cc' = 510
        allowlist = "a" * 253 + "\n" + "b" * 253 + "\n" + "cc"
        self.assertEqual(len(allowlist), 510)
        r = _save_allowlist(allowlist)
        self.assertEqual(r.status_code, 200,
                         f"Expected 200 for 510-char allowlist, got {r.status_code}: {r.text}")
        self.assertTrue(r.json().get("ok"), f"Expected ok=true: {r.json()}")


class TestHttpRequestInvoke(unittest.TestCase):
    """
    HTTPCFG-050~053: cap_http_request 调用行为测试.

    通过 POST /api/cap/invoke 验证 allowlist 运行时拦截逻辑。
    不依赖外网连通性：仅测试 allowlist 拦截（发生在网络请求之前），
    对于"未被拦截"的 case 只验证错误不来自 allowlist。
    """

    INVOKE_URL = BOARD_BASE_URL + "/api/cap/invoke"

    @classmethod
    def setUpClass(cls):
        cls._original = _get_allowlist()

    @classmethod
    def tearDownClass(cls):
        if cls._original is not None:
            try:
                _save_allowlist(cls._original)
            except Exception:
                pass

    def _invoke_http(self, url, method="GET"):
        """调用 http_request cap，返回 (http_status, parsed_json_body)。
        cap 成功 → HTTP 200；cap 失败（allowlist 拦截、参数错误等）→ HTTP 500。"""
        r = requests.post(
            self.INVOKE_URL,
            json={"cap": "http_request", "input": {"method": method, "url": url}},
            timeout=HTTP_TIMEOUT,
        )
        self.assertIn(r.status_code, [200, 500],
                      f"Unexpected HTTP status {r.status_code}: {r.text[:200]}")
        return r.status_code, r.json()

    def _is_allowlist_error(self, body: dict) -> bool:
        return "error" in body and "allowlist" in body["error"].lower()

    def test_HTTPCFG_050_empty_allowlist_blocks_any_request(self):
        """HTTPCFG-050: allowlist='' 时所有请求被拦截，HTTP 500 + allowlist 错误。"""
        _save_allowlist("")
        status, body = self._invoke_http("http://ip-api.com/json")
        self.assertEqual(status, 500, f"Expected HTTP 500 for blocked request, got {status}")
        self.assertTrue(self._is_allowlist_error(body),
                        f"Expected allowlist error, got: {body}")

    def test_HTTPCFG_051_star_allowlist_passes_request(self):
        """HTTPCFG-051: allowlist='*' 时请求通过 allowlist，HTTP 200 + status_code 字段。"""
        _save_allowlist("*")
        status, body = self._invoke_http("http://ip-api.com/json")
        self.assertEqual(status, 200,
                         f"Expected HTTP 200 with '*' allowlist, got {status}: {body}")
        self.assertFalse(self._is_allowlist_error(body),
                         f"Got unexpected allowlist block with '*': {body}")
        self.assertIn("status_code", body, f"Expected status_code in response: {body}")

    def test_HTTPCFG_052_specific_rule_blocks_non_matching_host(self):
        """HTTPCFG-052: allowlist 只含 example.com，对 ip-api.com 被拦截，HTTP 500。"""
        _save_allowlist("example.com")
        status, body = self._invoke_http("http://ip-api.com/json")
        self.assertEqual(status, 500,
                         f"Expected HTTP 500 for blocked host, got {status}: {body}")
        self.assertTrue(self._is_allowlist_error(body),
                        f"Expected allowlist block, got: {body}")

    def test_HTTPCFG_053_exact_host_rule_passes_matching_request(self):
        """HTTPCFG-053: allowlist 含 ip-api.com，对该主机通过，HTTP 200。"""
        _save_allowlist("ip-api.com")
        status, body = self._invoke_http("http://ip-api.com/json")
        self.assertEqual(status, 200,
                         f"Expected HTTP 200 for matching host, got {status}: {body}")
        self.assertFalse(self._is_allowlist_error(body),
                         f"Got unexpected allowlist block: {body}")

    def test_HTTPCFG_054_wildcard_rule_blocks_non_matching_host(self):
        """HTTPCFG-054: allowlist='*.example.com'，对 ip-api.com 被拦截，HTTP 500。"""
        _save_allowlist("*.example.com")
        status, body = self._invoke_http("http://ip-api.com/json")
        self.assertEqual(status, 500,
                         f"Expected HTTP 500 for non-matching host, got {status}: {body}")
        self.assertTrue(self._is_allowlist_error(body),
                        f"Expected allowlist block, got: {body}")

    def test_HTTPCFG_055_wildcard_rule_passes_matching_subdomain(self):
        """HTTPCFG-055: allowlist='*.ip-api.com'，对 pro.ip-api.com 通过，HTTP 200。"""
        _save_allowlist("*.ip-api.com")
        status, body = self._invoke_http("http://pro.ip-api.com/json")
        self.assertEqual(status, 200,
                         f"Expected HTTP 200 for matching subdomain, got {status}: {body}")
        self.assertFalse(self._is_allowlist_error(body),
                         f"Got unexpected allowlist block for matching subdomain: {body}")

    def test_HTTPCFG_056_invalid_method_rejected(self):
        """HTTPCFG-056: 非法 method → HTTP 500 + error 包含 method。"""
        _save_allowlist("*")
        r = requests.post(
            self.INVOKE_URL,
            json={"cap": "http_request",
                  "input": {"method": "INVALID", "url": "http://example.com/"}},
            timeout=HTTP_TIMEOUT,
        )
        self.assertEqual(r.status_code, 500,
                         f"Expected HTTP 500 for invalid method, got {r.status_code}: {r.text}")
        body = r.json()
        self.assertIn("error", body, f"Expected error for invalid method: {body}")
        self.assertIn("method", body["error"].lower(),
                      f"Error should mention method, got: {body['error']}")

    def test_HTTPCFG_057_invalid_url_rejected(self):
        """HTTPCFG-057: 非 http(s):// URL → HTTP 500 + error。"""
        _save_allowlist("*")
        r = requests.post(
            self.INVOKE_URL,
            json={"cap": "http_request",
                  "input": {"method": "GET", "url": "ftp://example.com/file"}},
            timeout=HTTP_TIMEOUT,
        )
        self.assertEqual(r.status_code, 500,
                         f"Expected HTTP 500 for ftp:// URL, got {r.status_code}: {r.text}")
        body = r.json()
        self.assertIn("error", body, f"Expected error for ftp:// URL: {body}")

    def test_HTTPCFG_058_rule_with_port_matches_host(self):
        """HTTPCFG-058: 规则含端口 ip-api.com:80 应匹配 http://ip-api.com/json，HTTP 200。

        回归测试 Bug#2：allowlist_permit() 匹配前必须同时剥除规则的 port，
        否则 'ip-api.com:80' 永远无法匹配已剥 port 的 'ip-api.com'。
        """
        _save_allowlist("ip-api.com:80")
        status, body = self._invoke_http("http://ip-api.com/json")
        self.assertEqual(status, 200,
                         f"Rule with port should match host; got {status}: {body}")
        self.assertFalse(self._is_allowlist_error(body),
                         f"Got unexpected allowlist block for port rule: {body}")

    def test_HTTPCFG_059_rule_port_is_hostname_only_match(self):
        """HTTPCFG-059: 规则端口号不限制实际端口，匹配仅按主机名。

        'ip-api.com:8443' 规则应放行 http://ip-api.com/json (port 80)，
        因为规则里的 port 会被剥除，只匹配主机名部分。
        """
        _save_allowlist("ip-api.com:8443")
        status, body = self._invoke_http("http://ip-api.com/json")
        self.assertEqual(status, 200,
                         f"Rule port should be stripped; port mismatch should not block; got {status}: {body}")
        self.assertFalse(self._is_allowlist_error(body),
                         f"Got unexpected allowlist block: {body}")

    def test_HTTPCFG_060_rule_with_port_blocks_non_matching_host(self):
        """HTTPCFG-060: 规则 example.com:443 对 ip-api.com 仍然拦截，HTTP 500。"""
        _save_allowlist("example.com:443")
        status, body = self._invoke_http("http://ip-api.com/json")
        self.assertEqual(status, 500,
                         f"Expected HTTP 500 for non-matching host, got {status}: {body}")
        self.assertTrue(self._is_allowlist_error(body),
                        f"Expected allowlist block, got: {body}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
