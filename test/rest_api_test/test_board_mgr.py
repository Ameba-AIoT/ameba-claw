"""
BRD-001 to BRD-033: Board Management capability tests.

Tests cap_board_mgr via POST /api/cap/invoke.
Response is returned directly as JSON (no wrapping).

Covers:
  board_list_devices          (BRD-001 to BRD-005)
  board_get_device            (BRD-006 to BRD-012)
  board_query_peripheral      (BRD-013 to BRD-020)
  board_schema                (BRD-021 to BRD-024)
  board_reload                (BRD-025 to BRD-027)
  VFS board customization     (BRD-028 to BRD-030)
    BRD-028: 无VFS board.json时加载编译期默认板
    BRD-029: 写入自定义VFS board.json后reload立即生效
    BRD-030: $extends继承父板设备列表
  Lua module chip filter      (BRD-031 to BRD-033)
    BRD-031: RTL8721F 已知外设的 Lua 模块 chip_ok=true（filter 已激活）
    BRD-032: RTL8721F.json 新增外设 captouch/thermal/basictimer chip_ok=true
    BRD-033: chip_ok 与 enabled/locked 字段一致性
"""
import unittest
import requests
import json
import sys
import os
sys.path.insert(0, os.path.dirname(__file__))
from config import BOARD_BASE_URL, HTTP_TIMEOUT


def board_post(path, **kwargs):
    return requests.post(BOARD_BASE_URL + path, timeout=HTTP_TIMEOUT, **kwargs)


def cap_invoke(cap_name, **input_fields):
    """Call POST /api/cap/invoke, return (status_code, parsed_json_or_None)."""
    r = board_post("/api/cap/invoke", json={"cap": cap_name, "input": input_fields})
    try:
        data = r.json()
    except Exception:
        data = None
    return r.status_code, data


# ---------------------------------------------------------------------------
# BRD-001 to BRD-005 — board_list_devices
# ---------------------------------------------------------------------------

class TestBoardListDevices(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.status, cls.data = cap_invoke("board_list_devices")

    def test_BRD_001_list_devices_returns_200(self):
        """board_list_devices returns HTTP 200."""
        self.assertEqual(self.status, 200,
            f"board_list_devices returned {self.status}: {self.data}")

    def test_BRD_002_list_devices_has_board_field(self):
        """Response contains non-empty 'board' string field."""
        self.assertEqual(self.status, 200)
        self.assertIn("board", self.data, f"Missing 'board': {self.data}")
        self.assertIsInstance(self.data["board"], str)
        self.assertGreater(len(self.data["board"]), 0, "'board' should not be empty")

    def test_BRD_003_list_devices_has_devices_array(self):
        """Response contains 'devices' array field."""
        self.assertEqual(self.status, 200)
        self.assertIn("devices", self.data, f"Missing 'devices': {self.data}")
        self.assertIsInstance(self.data["devices"], list)

    def test_BRD_004_list_devices_device_fields(self):
        """Each device entry has id, name, type, chip, interface fields."""
        self.assertEqual(self.status, 200)
        for dev in self.data.get("devices", []):
            for field in ("id", "name", "type", "chip", "interface"):
                self.assertIn(field, dev,
                    f"Device {dev.get('id','?')} missing '{field}': {dev}")

    def test_BRD_005_list_devices_known_board_name(self):
        """Board name is one of the known builtin boards."""
        self.assertEqual(self.status, 200)
        KNOWN = {"EV721FL0_R03", "EV721FL0_R03_BreadBoard"}
        board_name = self.data.get("board", "")
        self.assertIn(board_name, KNOWN,
            f"Board '{board_name}' not in known boards {KNOWN}")


# ---------------------------------------------------------------------------
# BRD-006 to BRD-012 — board_get_device
# ---------------------------------------------------------------------------

class TestBoardGetDevice(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        _, list_data = cap_invoke("board_list_devices")
        devices = list_data.get("devices", []) if list_data else []
        cls.device_ids = [d["id"] for d in devices if "id" in d]
        cls.has_oled = "oled" in cls.device_ids
        cls.btn = next((d for d in cls.device_ids if d.startswith("btn_")), None)

    def test_BRD_006_get_device_valid_id_returns_200(self):
        """board_get_device with a valid id returns HTTP 200."""
        if not self.device_ids:
            self.skipTest("No devices on this board")
        status, _ = cap_invoke("board_get_device", id=self.device_ids[0])
        self.assertEqual(status, 200)

    def test_BRD_007_get_device_response_has_required_fields(self):
        """Response for a valid device has id, name, type, chip fields."""
        if not self.device_ids:
            self.skipTest("No devices on this board")
        dev_id = self.device_ids[0]
        status, data = cap_invoke("board_get_device", id=dev_id)
        self.assertEqual(status, 200)
        for field in ("id", "name", "type", "chip"):
            self.assertIn(field, data,
                f"Device response missing '{field}': {data}")
        self.assertEqual(data.get("id"), dev_id)

    def test_BRD_008_get_device_unknown_id_returns_error_field(self):
        """board_get_device with nonexistent id returns 200 with 'error' field."""
        status, data = cap_invoke("board_get_device", id="nonexistent_xyz_device")
        self.assertEqual(status, 200,
            f"Expected 200 with error field, got {status}: {data}")
        self.assertIn("error", data,
            f"Expected 'error' field for unknown device: {data}")

    def test_BRD_009_get_device_missing_id_returns_error_field(self):
        """board_get_device with no id returns 200 with 'error' field."""
        status, data = cap_invoke("board_get_device")
        self.assertEqual(status, 200,
            f"Expected 200 with error field, got {status}: {data}")
        self.assertIn("error", data,
            f"Expected 'error' field for missing id: {data}")

    def test_BRD_010_get_device_oled_has_inline_interface(self):
        """board_get_device(oled) returns inlined interface object with pin fields."""
        if not self.has_oled:
            self.skipTest("oled not on this board")
        status, data = cap_invoke("board_get_device", id="oled")
        self.assertEqual(status, 200)
        self.assertIn("interface", data,
            f"oled response missing 'interface': {data}")
        iface = data["interface"]
        self.assertIsInstance(iface, dict,
            f"interface should be a dict: {iface}")
        self.assertIn("instance", iface,
            f"interface missing 'instance': {iface}")
        self.assertIn("sda", iface, f"I2C interface missing 'sda': {iface}")
        self.assertIn("scl", iface, f"I2C interface missing 'scl': {iface}")

    def test_BRD_011_get_device_oled_has_params(self):
        """board_get_device(oled) has non-empty params object."""
        if not self.has_oled:
            self.skipTest("oled not on this board")
        status, data = cap_invoke("board_get_device", id="oled")
        self.assertEqual(status, 200)
        self.assertIn("params", data, f"oled missing 'params': {data}")
        self.assertIsInstance(data["params"], dict)
        self.assertGreater(len(data["params"]), 0, "oled params should not be empty")

    def test_BRD_012_get_device_button_type_is_button(self):
        """board_get_device for a button device returns type='button'."""
        if not self.btn:
            self.skipTest("No button devices on this board")
        status, data = cap_invoke("board_get_device", id=self.btn)
        self.assertEqual(status, 200)
        self.assertEqual(data.get("type"), "button",
            f"Expected type=button for {self.btn}, got: {data.get('type')}")


# ---------------------------------------------------------------------------
# BRD-013 to BRD-020 — board_query_peripheral
# ---------------------------------------------------------------------------

class TestBoardQueryPeripheral(unittest.TestCase):

    def test_BRD_013_query_i2c_supported(self):
        """board_query_peripheral(i2c) returns supported=true."""
        status, data = cap_invoke("board_query_peripheral", peripheral="i2c")
        self.assertEqual(status, 200)
        self.assertTrue(data.get("supported"),
            f"i2c should be supported: {data}")

    def test_BRD_014_query_i2c_has_I2C0_and_I2C1(self):
        """board_query_peripheral(i2c) instances contains I2C0 and I2C1."""
        status, data = cap_invoke("board_query_peripheral", peripheral="i2c")
        self.assertEqual(status, 200)
        instances = data.get("instances", {})
        self.assertIsInstance(instances, dict)
        self.assertIn("I2C0", instances, f"I2C0 missing in instances: {instances}")
        self.assertIn("I2C1", instances, f"I2C1 missing in instances: {instances}")

    def test_BRD_015_query_i2c_instance_status_valid(self):
        """Each i2c instance has status of 'free' or 'occupied'."""
        status, data = cap_invoke("board_query_peripheral", peripheral="i2c")
        self.assertEqual(status, 200)
        for inst_name, inst in data.get("instances", {}).items():
            self.assertIn("status", inst,
                f"Instance {inst_name} missing 'status': {inst}")
            self.assertIn(inst["status"], ("free", "occupied"),
                f"Instance {inst_name} invalid status: {inst['status']}")

    def test_BRD_016_query_i2c_occupied_instance_has_config(self):
        """Occupied i2c instance has 'config' with sda/scl (BreadBoard only)."""
        status, data = cap_invoke("board_query_peripheral", peripheral="i2c")
        self.assertEqual(status, 200)
        occupied = {k: v for k, v in data.get("instances", {}).items()
                    if v.get("status") == "occupied"}
        if not occupied:
            self.skipTest("No occupied i2c instances on this board")
        for inst_name, inst in occupied.items():
            self.assertIn("config", inst,
                f"Occupied instance {inst_name} missing 'config': {inst}")
            cfg = inst["config"]
            self.assertIn("sda", cfg, f"I2C config missing 'sda': {cfg}")
            self.assertIn("scl", cfg, f"I2C config missing 'scl': {cfg}")

    def test_BRD_017_query_i2c_has_available_pins(self):
        """board_query_peripheral(i2c) returns non-empty available_pins array."""
        status, data = cap_invoke("board_query_peripheral", peripheral="i2c")
        self.assertEqual(status, 200)
        self.assertIn("available_pins", data,
            f"Missing 'available_pins': {data}")
        pins = data["available_pins"]
        self.assertIsInstance(pins, list)
        self.assertGreater(len(pins), 0, "available_pins should not be empty")

    def test_BRD_018_query_rtc_no_available_pins(self):
        """board_query_peripheral(rtc) is supported but has no available_pins (no_pins=true)."""
        status, data = cap_invoke("board_query_peripheral", peripheral="rtc")
        self.assertEqual(status, 200)
        self.assertTrue(data.get("supported"),
            f"rtc should be supported: {data}")
        self.assertNotIn("available_pins", data,
            f"rtc has no_pins=true, available_pins should be absent: {data}")

    def test_BRD_019_query_unsupported_peripheral_returns_false(self):
        """board_query_peripheral(can) returns supported=false (CAN not on RTL8721F)."""
        status, data = cap_invoke("board_query_peripheral", peripheral="can")
        self.assertEqual(status, 200)
        self.assertFalse(data.get("supported"),
            f"'can' should not be supported on RTL8721F: {data}")

    def test_BRD_020_query_by_instance_name_resolves_type(self):
        """board_query_peripheral(SPI0) resolves to peripheral type 'spi'."""
        status, data = cap_invoke("board_query_peripheral", peripheral="SPI0")
        self.assertEqual(status, 200)
        self.assertTrue(data.get("supported"),
            f"SPI0 should resolve to supported spi: {data}")
        self.assertEqual(data.get("peripheral"), "spi",
            f"peripheral field should be 'spi' for SPI0 lookup: {data}")


# ---------------------------------------------------------------------------
# BRD-021 to BRD-024 — board_schema
# ---------------------------------------------------------------------------

class TestBoardSchema(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.status, cls.data = cap_invoke("board_schema")

    def test_BRD_021_schema_returns_200(self):
        """board_schema returns HTTP 200."""
        self.assertEqual(self.status, 200,
            f"board_schema returned {self.status}: {self.data}")

    def test_BRD_022_schema_chips_list_has_RTL8721F(self):
        """Response contains chips array with RTL8721F."""
        self.assertIn("chips", self.data, f"Missing 'chips': {self.data}")
        chips = self.data["chips"]
        self.assertIsInstance(chips, list)
        self.assertIn("RTL8721F", chips, f"RTL8721F not in chips: {chips}")

    def test_BRD_023_schema_builtin_boards_has_known_boards(self):
        """Response contains builtin_boards array with known board names."""
        self.assertIn("builtin_boards", self.data,
            f"Missing 'builtin_boards': {self.data}")
        boards = self.data["builtin_boards"]
        self.assertIsInstance(boards, list)
        self.assertGreater(len(boards), 0, "builtin_boards should not be empty")
        KNOWN = {"EV721FL0_R03", "EV721FL0_R03_BreadBoard"}
        for b in boards:
            self.assertIn(b, KNOWN, f"Unexpected builtin board: {b}")

    def test_BRD_024_schema_has_authoring_schema_object(self):
        """Response contains authoring_schema as a non-empty dict."""
        self.assertIn("authoring_schema", self.data,
            f"Missing 'authoring_schema': {self.data}")
        schema = self.data["authoring_schema"]
        self.assertIsInstance(schema, dict,
            f"authoring_schema should be a dict: {type(schema)}")
        self.assertGreater(len(schema), 0, "authoring_schema should not be empty")


# ---------------------------------------------------------------------------
# BRD-025 to BRD-027 — board_reload
# ---------------------------------------------------------------------------

class TestBoardReload(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        _, list_data = cap_invoke("board_list_devices")
        cls.expected_devices = len(list_data.get("devices", [])) if list_data else 0
        _, iface_data = cap_invoke("board_query_peripheral", peripheral="i2c")
        # Count occupied interfaces as a proxy; just ensure reload is consistent
        cls.reload_status, cls.reload_data = cap_invoke("board_reload")

    def test_BRD_025_reload_returns_200(self):
        """board_reload returns HTTP 200."""
        self.assertEqual(self.reload_status, 200,
            f"board_reload returned {self.reload_status}: {self.reload_data}")

    def test_BRD_026_reload_response_has_success_and_board(self):
        """board_reload response has success=true and a board name."""
        self.assertEqual(self.reload_status, 200)
        self.assertTrue(self.reload_data.get("success"),
            f"Expected success=true: {self.reload_data}")
        self.assertIn("board", self.reload_data,
            f"Missing 'board' in reload response: {self.reload_data}")
        self.assertGreater(len(self.reload_data.get("board", "")), 0)

    def test_BRD_027_reload_device_count_matches_list(self):
        """board_reload reports device count consistent with board_list_devices."""
        self.assertEqual(self.reload_status, 200)
        self.assertIn("devices", self.reload_data,
            f"Missing 'devices' count in reload response: {self.reload_data}")
        self.assertIn("interfaces", self.reload_data,
            f"Missing 'interfaces' count in reload response: {self.reload_data}")
        self.assertIsInstance(self.reload_data["devices"], (int, float))
        reload_n = int(self.reload_data["devices"])
        self.assertEqual(reload_n, self.expected_devices,
            f"reload n_devices={reload_n} differs from list count={self.expected_devices}")


# ---------------------------------------------------------------------------
# BRD-028 to BRD-030 — VFS board customization
# Requirement: 编译期选择board型号 / 运行时VFS自定义board.json继承&覆盖
# ---------------------------------------------------------------------------

COMPILE_TIME_DEFAULT_BOARD = "EV721FL0_R03_BreadBoard"  # built with -DBOARD=EV721FL0_R03_BreadBoard
VFS_BOARD_PATH = "/board.json"


class TestVfsBoardCustomization(unittest.TestCase):

    def _vfs_delete(self):
        requests.delete(BOARD_BASE_URL + "/api/files",
                        params={"path": VFS_BOARD_PATH}, timeout=HTTP_TIMEOUT)

    def _vfs_write(self, content_dict):
        return requests.put(BOARD_BASE_URL + "/api/files/content",
                            params={"path": VFS_BOARD_PATH},
                            data=json.dumps(content_dict).encode("utf-8"),
                            timeout=HTTP_TIMEOUT)

    def tearDown(self):
        """Restore default state: delete custom VFS board.json and reload."""
        self._vfs_delete()
        cap_invoke("board_reload")

    def test_BRD_028_vfs_absent_loads_compile_time_default(self):
        """When VFS board.json is absent, board_reload loads the compile-time default board.

        Requirement: 编译期选择当前board型号
        Built with -DBOARD=EV721FL0_R03_BreadBoard, so that is the expected default.
        """
        self._vfs_delete()
        status, data = cap_invoke("board_reload")
        self.assertEqual(status, 200, f"board_reload failed: {data}")
        self.assertEqual(data.get("board"), COMPILE_TIME_DEFAULT_BOARD,
            f"Expected compile-time default '{COMPILE_TIME_DEFAULT_BOARD}', "
            f"got '{data.get('board')}'")

    def test_BRD_029_vfs_custom_board_reload_takes_effect(self):
        """Writing a custom board.json to VFS and calling board_reload makes it active.

        Requirement: 运行时允许用户自定义board扩展 /
                     运行时允许用户通过对话交互修改 vfs:board.json
        """
        custom = {
            "$chip": "RTL8721F",
            "$extends": "EV721FL0_R03",
            "board": {
                "name": "BRD029_TestBoard",
                "chip": "RTL8721FLM",
                "description": "Automated test custom board"
            },
            "devices": [{
                "id": "brd029_led",
                "name": "BRD029 Test LED",
                "type": "led",
                "params": {"pin": "PA_0"}
            }]
        }
        r = self._vfs_write(custom)
        self.assertIn(r.status_code, [200, 204],
            f"VFS write failed with {r.status_code}: {r.text[:200]}")

        reload_status, reload_data = cap_invoke("board_reload")
        self.assertEqual(reload_status, 200, f"board_reload failed: {reload_data}")
        self.assertEqual(reload_data.get("board"), "BRD029_TestBoard",
            f"Expected custom board name, got: {reload_data.get('board')}")

        _, list_data = cap_invoke("board_list_devices")
        ids = [d["id"] for d in list_data.get("devices", [])]
        self.assertIn("brd029_led", ids,
            f"Custom device 'brd029_led' not found in devices: {ids}")

    def test_BRD_030_vfs_board_extends_inherits_parent_devices(self):
        """Custom board.json with $extends inherits parent board devices.

        Requirement: 运行时允许用户自定义board扩展 — board_merge 继承规则
        """
        custom = {
            "$chip": "RTL8721F",
            "$extends": "EV721FL0_R03_BreadBoard",
            "board": {
                "name": "BRD030_InheritBoard",
                "chip": "RTL8721FLM"
            },
            "devices": [{
                "id": "brd030_sensor",
                "name": "BRD030 Extra Sensor",
                "type": "sensor",
                "params": {"pin": "PA_0"}
            }]
        }
        r = self._vfs_write(custom)
        self.assertIn(r.status_code, [200, 204],
            f"VFS write failed with {r.status_code}: {r.text[:200]}")

        cap_invoke("board_reload")
        _, list_data = cap_invoke("board_list_devices")
        ids = [d["id"] for d in list_data.get("devices", [])]

        self.assertIn("brd030_sensor", ids,
            f"Custom device 'brd030_sensor' not found: {ids}")
        self.assertIn("oled", ids,
            f"Inherited parent device 'oled' missing after $extends: {ids}")


# ---------------------------------------------------------------------------
# BRD-031 to BRD-033 — Lua module chip filter (side-effect of cap_board_mgr)
#
# cap_board_mgr_init() installs cap_board_mgr_chip_has_peripheral as the chip
# filter for lua_module_registry.  The GET /api/lua/modules response reflects
# the filter result in the chip_ok field.
#
# Known test blindspot: chip_ok=false path is not exercisable on RTL8721F
# because every peripheral used by any Lua module is listed in RTL8721F.json.
# We verify chip_ok=true for a representative set instead.
# ---------------------------------------------------------------------------

def _get_lua_modules():
    """GET /api/lua/modules → list of module dicts."""
    r = requests.get(BOARD_BASE_URL + "/api/lua/modules", timeout=HTTP_TIMEOUT)
    r.raise_for_status()
    return r.json().get("modules", [])


def _find_lua_module(modules, module_id):
    for m in modules:
        if m.get("id") == module_id:
            return m
    return None


class TestLuaModulesChipFilter(unittest.TestCase):
    """BRD-031 to BRD-033: chip_ok correctness via cap_board_mgr chip filter."""

    @classmethod
    def setUpClass(cls):
        cls.modules = _get_lua_modules()

    def test_BRD_031_known_rtl8721f_peripherals_chip_ok_true(self):
        """BRD-031: RTL8721F 基础外设 (gpio/i2c/spi/uart) 对应 Lua 模块 chip_ok=true.

        这些外设一直在 RTL8721F.json 中。若 chip filter 未安装（s_chip_filter==NULL），
        lua_module_registry_chip_ok 也返回 true，所以此测试无法区分"filter 已装但正确"和
        "filter 未装"。BRD-032 通过验证新加外设来补充覆盖。
        """
        # HW modules that must be compiled-in and present on RTL8721F
        expected_true = ["gpio", "i2c", "spi", "uart"]
        for mod_id in expected_true:
            m = _find_lua_module(self.modules, mod_id)
            if m is None:
                continue  # compiled out via Kconfig — skip
            self.assertTrue(
                m["chip_ok"],
                f"Module '{mod_id}' should have chip_ok=true on RTL8721F, got: {m}"
            )

    def test_BRD_032_new_rtl8721f_json_entries_chip_ok_true(self):
        """BRD-032: RTL8721F.json 新增外设 captouch/thermal/basictimer 的 Lua 模块 chip_ok=true.

        这三个外设是本次改动新增到 RTL8721F.json 中的。chip_ok=true 意味着：
          1. cap_board_mgr 已加载板 JSON（s_model.loaded == true）
          2. chip filter 已被安装到 lua_module_registry
          3. 新 JSON 条目被正确解析
        如果 chip filter 未安装，chip_ok 也返回 true，无法区分——此 case 主要捕获
        "新 JSON 条目写错 key / filter 逻辑反转"这类错误。
        """
        new_peripherals = ["captouch", "thermal", "basictimer"]
        found_any = False
        for mod_id in new_peripherals:
            m = _find_lua_module(self.modules, mod_id)
            if m is None:
                continue  # compiled out via Kconfig — skip
            found_any = True
            self.assertTrue(
                m["chip_ok"],
                f"Module '{mod_id}' should have chip_ok=true (peripheral in RTL8721F.json), got: {m}"
            )
        if not found_any:
            self.skipTest("captouch/thermal/basictimer all compiled out — Kconfig test M4-LUA-01 covers trimming")

    def test_BRD_033_chip_ok_enabled_locked_consistency(self):
        """BRD-033: enabled 与 chip_ok/locked 字段一致.

        API 文档定义：
          enabled = chip_ok AND (locked OR NOT disabled_by_user)
          locked  = original_locked OR NOT chip_ok   (chip absent → force-locked)
        对当前全量固件（disabled 为空），所有 chip_ok=true 的非 locked 模块应 enabled=true。
        chip_ok=false 的模块应 locked=true（防止用户开启无法工作的模块）。
        """
        for m in self.modules:
            mid = m.get("id", "?")
            chip_ok = m.get("chip_ok")
            locked  = m.get("locked")
            enabled = m.get("enabled")

            if chip_ok is False:
                # chip absent → module must be locked (not user-togglable)
                self.assertTrue(
                    locked,
                    f"Module '{mid}' has chip_ok=false but locked=false — "
                    "should be force-locked when chip is absent"
                )
                # and must not be enabled
                self.assertFalse(
                    enabled,
                    f"Module '{mid}' has chip_ok=false but enabled=true — impossible"
                )
            else:
                # chip_ok=true: if not locked and not user-disabled, must be enabled
                # (we assume empty disabled list at test time — test_untested_endpoints
                #  restores disabled to "" after each of its tests)
                if not locked:
                    self.assertTrue(
                        enabled,
                        f"Module '{mid}' chip_ok=true, not locked, not disabled "
                        "→ should be enabled=true"
                    )


if __name__ == "__main__":
    unittest.main(verbosity=2)
