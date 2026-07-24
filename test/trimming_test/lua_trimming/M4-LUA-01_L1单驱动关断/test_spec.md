# M4-LUA-01 Lua 驱动 L1 关断（Kconfig，全量矩阵）

逐一验证每个 `CONFIG_CLAW_LUA_DRV_*=n`：编译干净，对应模块从 `/api/lua/modules`
响应的 `modules` 数组中消失（L1 关断 = 未编译，模块条目不存在），
`require()` 运行时失败，其余驱动不受影响。

---

## 背景

`CLAW_LUA_DRV_*` Kconfig 符号（`ameba_claw/Kconfig`，"Lua Peripheral Drivers" menu）
通过 `lua/CMakeLists.txt` 的 `_kconfig_disable` 宏关闭 `LUA_MOD_ENABLE_*` 开关，
使对应 `.c` 不被编译。关闭后：
- `lua_module_registry()` 返回的表里不再有该条目
- `GET /api/lua/modules` 响应的 `modules` 数组中不出现该模块 ID
- `require('<mod>')` 抛出错误（pcall 返回 false）

`GET /api/lua/modules` 当前响应格式：
```json
{
  "modules": [
    {"id": "gpio", "category": "hardware", "enabled": true, "locked": false, "chip_ok": true},
    ...
  ],
  "disabled": ""
}
```

L1 关断的模块不出现在 `modules` 数组（区别于 L2 运行时关断：模块仍在数组但 `enabled=false`）。

---

## 测试矩阵

| # | Kconfig 符号 | 模块 ID（`id` 字段）| Kconfig 依赖 | 默认 |
|---|---|---|---|---|
| 1 | `CLAW_LUA_DRV_I2C` | `i2c` + #12–#15 同时消失 ① | — | y |
| 2 | `CLAW_LUA_DRV_SPI` | `spi` | — | y |
| 3 | `CLAW_LUA_DRV_UART` | `uart` | — | y |
| 4 | `CLAW_LUA_DRV_PWM` | `pwm` | — | y |
| 5 | `CLAW_LUA_DRV_ADC` | `adc` | — | y |
| 6 | `CLAW_LUA_DRV_RTC` | `rtc` | — | y |
| 7 | `CLAW_LUA_DRV_IR` | `ir` | — | y |
| 8 | `CLAW_LUA_DRV_THERMAL` | `thermal` | — | y |
| 9 | `CLAW_LUA_DRV_CAPTOUCH` | `captouch` | — | y |
| 10 | `CLAW_LUA_DRV_BASICTIMER` | `basictimer` | — | y |
| 11 | `CLAW_LUA_DRV_LED_STRIP` | `led_strip` | — | y |
| 12 | `CLAW_LUA_DRV_IMU` | `imu` | `CLAW_LUA_DRV_I2C` | y |
| 13 | `CLAW_LUA_DRV_ENV_SENSOR` | `environmental_sensor` | `CLAW_LUA_DRV_I2C` | y |
| 14 | `CLAW_LUA_DRV_LIGHT_SENSOR` | `light_sensor` | `CLAW_LUA_DRV_I2C` | y |
| 15 | `CLAW_LUA_DRV_MAGNETOMETER` | `magnetometer` | `CLAW_LUA_DRV_I2C` | y |
| 16 | `CLAW_LUA_DRV_LVGL` | `lvgl` + `display` 同时消失 ② | — | y |

**跳过：**
- `CLAW_LUA_DRV_AUDIO`、`CLAW_LUA_DRV_USB_UVC`、`CLAW_LUA_DRV_USB_MSC`：Kconfig `default n`，标准构建已关断
- `CLAW_LUA_DRV_GPIO`：hidden symbol，硬性依赖 lua_module_event，不可用户关断

① `CLAW_LUA_DRV_I2C=n` 时，`depends on CLAW_LUA_DRV_I2C` 的 #12–#15 也一并消失；
验证时需确认 `i2c`、`imu`、`environmental_sensor`、`light_sensor`、`magnetometer` 全部缺席。

② `CLAW_LUA_DRV_LVGL=n` 时，cmake cascade 同时强制关闭 `display` 模块；验证时确认 `lvgl` 和 `display` 均缺席。

---

## 通用步骤（对每一条目循环执行）

### 1. 配置

```bash
./ameba.py menuconfig --set CLAW_LUA_DRV_<X>=n
```

### 2. 编译

```bash
./ameba.py build -a . 2>&1 | grep -E "error:|Build done|FAIL"
```

期望：只有 `Build done`，无 `error:`。

### 3. 烧录 + 等联网（≥ 30 s）

### 4. REST — 模块列表

```bash
# 替换 <mod_id> 为表中对应值（#2 需同时检查 imu 等 4 个）
curl -s http://127.0.0.1/api/lua/modules | python3 -c "
import sys, json
data = json.load(sys.stdin)
ids = {m['id'] for m in data.get('modules', [])}
print('TARGET absent from list:', '<mod_id>' not in ids)
print('spi    present:', 'spi'  in ids)
print('uart   present:', 'uart' in ids)
"
```

期望：`TARGET absent from list: True`；witness 模块（`adc`/`ir`/`thermal`/`basictimer` 中任取不在被测集合的 2 个）仍出现。

### 5. 运行时 require 验证

> 注：`cap_invoke` 属于非 LLM 调用，固件拒绝从 `vfs:/tmp/` 运行脚本；使用
> `/api/lua/content`（写入 `vfs:/skills/`）代替。

```bash
# 上传测试脚本（替换 <mod_id>）
curl -s -X PUT "http://127.0.0.1/api/lua/content?name=test_drv_off.lua" \
  --data 'function run(args) local ok = pcall(require, args.mod); return ok and (args.mod.."_LOADED") or (args.mod.."_BLOCKED") end'

# 调用 lua_run（替换 <mod_id>）
curl -s -X POST http://127.0.0.1/api/cap/invoke \
  -H "Content-Type: application/json" \
  -d '{"cap":"lua_run","input":{"path":"vfs:/skills/test_drv_off.lua","args":{"mod":"<mod_id>"}}}' \
  | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('output', d.get('stdout', '')))"

# 清理
curl -s -X DELETE "http://127.0.0.1/api/lua?name=test_drv_off.lua"
```

期望：输出包含 `<mod_id>_BLOCKED`。

### 6. 恢复

```bash
./ameba.py menuconfig --set CLAW_LUA_DRV_<X>=y
```

---

## Pass 标准（每条目）

- [ ] 编译零 error
- [ ] `GET /api/lua/modules` 的 `modules` 数组中目标模块 ID 不出现
- [ ] 至少两个其他驱动（如 `spi`、`uart`）仍出现在 `modules` 数组
- [ ] `lua_run` require 输出包含 `<mod_id>_BLOCKED`
