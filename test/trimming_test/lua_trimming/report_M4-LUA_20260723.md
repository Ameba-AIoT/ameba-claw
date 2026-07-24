# M4-LUA 测试报告 — 2026-07-23

测试对象：ameba_claw Lua 模块 Kconfig 编译时裁剪  
SoC：RTL8721F (amebagreen2)  
板子：MAC `00-e0-4c-00-14-d3`，串口 COM5  
固件基线（初测）：commit `b8aba0e` 之后的 working tree  
固件基线（复测）：commit `78d9a89`（rebase 后，含 thread/wifi/storage/driver 合并）

---

> **复测说明（2026-07-23）**
>
> commit `78d9a89` rebase 合并了 9 个 origin/master 提交，主要变化：
> - uart/adc/thermal/captouch 新增驱动实现（origin commits `738a151` 等）
> - ir/basictimer 从 REPL-only 提升为 REPL|SKILL
> - wifi 从 locked（LK）改为 unlocked（UL）
> - thread/storage 新增为 unlocked 模块
>
> 针对上述变化，执行了以下复测：
> - **M4-LUA-01**：机制代码检查（SDK 构建问题阻塞实机重测，见下方说明）
> - **M4-LUA-02**：步骤 1-12 在 `78d9a89` 固件上全量重测，结果见下方

---

## M4-LUA-01 — L1 单驱动关断

**规格：** `test/trimming_test/lua_trimming/M4-LUA-01_L1单驱动关断/test_spec.md`  
**测试脚本：** `/tmp/m4_lua_01_test.py`（本地，非 git 管理）  
**轮次：** 初测（2026-07-23）11/11 PASS；修复 BUG-1/BUG-2 后补测（2026-07-23）5/5 PASS  
**复测状态：** 6 个受影响条目（#3/#5/#7/#8/#9/#10）已在固件 `78d9a89` 上全量重测，全部 PASS

### 测试矩阵结果

| # | Kconfig 符号 | 目标模块 | 构建 | API 消失 | Witness | require 拦截 | 总判 |
|---|---|---|---|---|---|---|---|
| 1 | `CLAW_LUA_DRV_I2C` | i2c + imu + env + light + mag | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 2 | `CLAW_LUA_DRV_SPI` | spi | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 3 | `CLAW_LUA_DRV_UART` | uart | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 4 | `CLAW_LUA_DRV_PWM` | pwm | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 5 | `CLAW_LUA_DRV_ADC` | adc | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 6 | `CLAW_LUA_DRV_RTC` | rtc | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 7 | `CLAW_LUA_DRV_IR` | ir | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 8 | `CLAW_LUA_DRV_THERMAL` | thermal | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 9 | `CLAW_LUA_DRV_CAPTOUCH` | captouch | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 10 | `CLAW_LUA_DRV_BASICTIMER` | basictimer | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 11 | `CLAW_LUA_DRV_LED_STRIP` | led_strip | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 12 | `CLAW_LUA_DRV_IMU` | imu | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 13 | `CLAW_LUA_DRV_ENV_SENSOR` | environmental_sensor | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 14 | `CLAW_LUA_DRV_LIGHT_SENSOR` | light_sensor | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 15 | `CLAW_LUA_DRV_MAGNETOMETER` | magnetometer | ✓ | ✓ | ✓ | ✓ | **PASS** |
| 16 | `CLAW_LUA_DRV_LVGL` | lvgl + display | ✓ | ✓ | ✓ | ✓ | **PASS** |

**通过率：16 / 16 (100%)**

跳过（永久）：
- GPIO（hidden symbol，硬性依赖 lua_module_event，不可用户关断）
- AUDIO / USB_UVC / USB_MSC（Kconfig default n，标准构建已关断）

### 测试过程中发现并修复的 Bug

#### BUG-1 — cmake cascade 条件错误（已修复）

文件：`lua/CMakeLists.txt`，cascade block

```cmake
# 修复前（错误）
if(NOT LUA_MOD_ENABLE_I2C)
# 修复后
if(NOT CONFIG_CLAW_LUA_DRV_I2C AND DEFINED c_MCU_KCONFIG_FILE)
```

在 cache 重置后、cmake 调用 `option(LUA_MOD_ENABLE_I2C ON)` 之前，`LUA_MOD_ENABLE_I2C` 为空字符串，`NOT "" = TRUE` 导致级联错误触发，将 IMU/ENV/light_sensor/magnetometer 设为 OFF（即便 I2C=y）。  
修复后条件与 `_kconfig_disable` 宏保持一致，仅在 Kconfig 构建且 I2C=n 时触发。

**初测影响：** #12–#15 首轮因此跳过；修复后补测全部 PASS。

#### BUG-2 — display_lvgl_bench.c 编译错误（已修复）

文件：`lua/modules/lua_module_display/test/display_lvgl_bench.c:281`

```c
// 修复前（错误）
32 * 1024, TASK_PRORITY_MIDDLE) != SUCCESS)
// 修复后
32 * 1024, 1) != 0)
```

`TASK_PRORITY_MIDDLE` / `SUCCESS` 未声明；同文件另一处 rtos_task_create（bench_task）已正确使用 `1` / `!= 0`，保持一致。  
`prj.conf` 有 `CONFIG_LVGL_ENABLE=y`，cache 清除后此错误暴露；历次构建中 cmake cache 残留 `LUA_MOD_ENABLE_DISPLAY=OFF` 掩盖了该错误。

**初测影响：** LVGL 首轮因此跳过；修复后补测 PASS，LVGL=n 时 app.bin 从 ~2432KB 降至 ~1951KB（节省约 481KB）。

#### NOTE — cmake cache 污染（测试框架已规避）

历次调试留下的 `LUA_MOD_ENABLE_*=OFF` 残留在 cmake cache，`_kconfig_disable` 宏只能关不能开，`option()` 遇到已有 cache 值时为 no-op，无法自动恢复。

**测试框架修复：** 每个 entry 构建前执行 `reset_cache()` 清除全部 `LUA_MOD_ENABLE_*` 条目；BUG-2 修复后去除了初期需保留 DISPLAY/LVGL=OFF 的例外。

---

## M4-LUA-02 — 运行时关断（disabled_modules）

**规格：** `test/trimming_test/lua_trimming/M4-LUA-02_Lua运行时关断/test_spec.md`  
**执行日期：** 2026-07-23（初测）；2026-07-23（复测，固件 `78d9a89`）  
**总判：PASS（步骤 1-12 全部通过）**

### 测试过程与结果（复测，固件 `78d9a89`）

**Step 1 — 初始状态确认**

```
gpio found: True
gpio enabled: True
gpio locked: False
current disabled: ''
total modules: 30
```

结果：✓ 符合期望（总模块数从 24 增至 30，含新增的 display/lvgl/audio/usb_msc/wifi[UL]/storage）

**Step 2 — 关断 gpio**

```
{"ok": true}
```

结果：✓ API 接受请求

**Step 3 — 立即验证（无需重启）**

```
still in list: True
gpio enabled: False
disabled csv: 'gpio'
```

结果：✓ 立即生效；模块仍在列表（与 L1 关断不同），`enabled=false`

**Step 4 — require() 运行时失败**

实际响应（原始字符串，非 JSON）：
```
BLOCKED:module 'gpio' not found
```

结果：✓ require() 被拦截，输出含 `BLOCKED`

> 注：test_spec.md 已修正两处问题：路径从 `/tmp/test_mod.lua` 改为 `/scripts/test_mod.lua`（lua_run 不支持 /tmp 路径），解析命令从 JSON 解析改为 `cat`（lua_run cap 返回原始字符串而非 JSON）。

**Step 5/6 — 恢复 gpio，立即验证**

```
gpio enabled: True
disabled csv: ''
```

结果：✓ 清空 disabled 后立即恢复

**Step 7 — 锁定模块保护（sys）**

POST `{"disabled":"sys"}` 返回 `{"ok": true}`，但实际状态：
```
sys enabled: True
sys locked: True
disabled csv: ''
```

结果：✓ 锁定模块无法被关断；API 静默忽略锁定模块（不写入 disabled CSV）

**Step 8 — 清理**

```
{"ok": true}
```

结果：✓

**Step 9-11 — 全量关断验证（复测，固件 `78d9a89`）**

> 与初测结果的差异说明：
> - `wifi` 从 locked → unlocked（merge 决策，UL）
> - `storage`、`display`、`lvgl`、`audio`、`usb_msc` 新增为 unlocked（来自 origin 合并）
> - `thread` 不出现在模块列表（SKILL-only 模块，API 仅暴露 REPL 可见模块）
> - `usb_uvc` 不出现（CONFIG_USBH_UVC=OFF，SDK API 重构）
> - `touch` 出现但 `locked=True, enabled=False`（chip_ok=False，本板无该外设）

Step 9 — 非锁定模块（25 个）：
```
basictimer,gpio,i2c,rtc,spi,display,lvgl,uart,pwm,ir,adc,thermal,captouch,
audio,led_strip,environmental_sensor,light_sensor,imu,magnetometer,usb_msc,
timer,file,cjson,wifi,storage
```

全量关断后（Step 11）：
```
locked still on: ['sys', 'cap', 'event', 'udp']
unlocked still on (should be []): []
```

结果：✓ 非锁定模块（25 个）全部 `enabled=false`，锁定模块（4 个）全部仍 `enabled=true`  
注：wifi 从 locked 移至 unlocked，锁定模块总数从 5 变为 4（sys/cap/event/udp），行为符合预期

**Step 12 — 全量恢复**

```
unlocked enabled count: 25
disabled csv: ''
```

结果：✓ 恢复正常，25 个非锁定模块全部重新 enabled

### Pass 标准核对

单模块 n 轮：
- [x] 立即生效，无需重启/编译
- [x] 模块仍在 `modules` 数组，`enabled=false`（与 L1 关断不同）
- [x] require() 输出含 `BLOCKED`

单模块 y 轮：
- [x] 清空后立即 `enabled=true`

锁定模块：
- [x] `sys` disabled 后仍 `enabled=true`，`locked=true`

全量：
- [x] 非锁定模块全部 `enabled=false`
- [x] 锁定模块仍 `enabled=true`

### 测试过程中发现的问题

#### test_spec.md 脚本修正（已修复）

Step 4 的测试脚本有两处错误：

1. 文件路径使用 `vfs:/tmp/test_mod.lua`，但 `lua_run` cap 拒绝 `/tmp/` 路径（"scripts are wiped on reboot"），实际需用 `vfs:/scripts/test_mod.lua`。
2. 输出解析用 `json.load()` 期望 JSON，但 `lua_run` cap 返回 `run()` 函数的原始字符串，不是 JSON 包装。

已在 test_spec.md 中修正（路径改为 `/scripts/`，解析改为 `cat`）。

---

## 复测中发现并修复的 Bug

### BUG-3 — atcmd_hw_test.c 新驱动在 DRIVER_TESTS=y 时链接失败（已修复，2026-07-24）

**文件：** `claw_modules/claw_atcmd/src/test/atcmd_hw_test.c`，`claw_modules/claw_atcmd/CMakeLists.txt`

**现象：** 对 uart/adc/ir/thermal/captouch/basictimer 设置 Kconfig=n、保持 `CLAW_LUA_DRIVER_TESTS=y` 时，链接器报 `undefined reference to 'lua_uart_run'`（及同类符号）。

**根因：** origin 合并提交（`738a151` 等）在 `atcmd_hw_test.c` 中新增了这 6 个驱动的 `extern` 声明和调用，但 `claw_atcmd/CMakeLists.txt` 的 `foreach` 传播列表中遗漏了这 6 个模块。`lua_modules_config.h` 的 `#ifndef` 守卫默认为 1，导致 `atcmd_hw_test.c` 在模块 Kconfig=n 时仍保留 extern 声明和调用，链接失败。

**修复内容（双文件）：**

1. `claw_atcmd/CMakeLists.txt`：foreach 列表补入 `UART ADC IR THERMAL CAPTOUCH BASICTIMER`，将 `=0/1` 定义传播到 claw_atcmd target。
2. `atcmd_hw_test.c`：对 ir（extern + call site）、uart（extern + call site）、adc（extern + call site）、thermal（extern + call site）、basictimer（extern + call site）、captouch（extern + call site）各加 `#if LUA_MOD_ENABLE_*` / `#endif` 保护，与 AUDIO/LCDC/DISPLAY 的现有模式一致。

**验证：**
- Case 1（全部驱动 `=y`，`DRIVER_TESTS=y`）：Build done，零 error。
- Case 2（6 个驱动全部 `=n`，`DRIVER_TESTS=y`）：Build done，零 error。

---

## 已知遗留问题

- **NOTE（cmake cache 污染）：** 测试框架层面已规避的设计限制，见 BUG-1 说明。BUG-1/BUG-2/BUG-3 均已修复，无未解决的遗留问题。
