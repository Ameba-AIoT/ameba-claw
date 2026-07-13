# agent_auto_test — 外设物理注入的自动化测试流程

本目录放**主机侧**的自动化测试流程与 AT 命令清单,用于驱动"测试板(TESTER)"
对"被测板(DUT)"做**物理**激励,观测 ameba-claw harness 的真实行为。
目前覆盖 GPIO(按键模拟),未来可扩展其他外设。

固件侧的命令实现见 **本目录** `atcmd_gpio_ctrl.c`(`AT+CLAW=gpio_ctrl`);
视觉/整体闭环背景见 `auto_perf/vqa_pipeline.md` §6 与 `auto_perf/closed_loop_arch.md`。

> **⚠️ 编译开关(默认关闭,不进 release):** 本目录的测试桩由宏 `CLAW_AGENT_AUTO_TEST`
> 门控,**默认不编译**(release 镜像零成本)。要让 `gpio_ctrl` 进固件,编译时打开:
> ```
> source env.sh
> CLAW_AGENT_AUTO_TEST=1 ./ameba.py build -q
> ```
> (`-q` 必加,见 CLAUDE.md。)开启时 `claw_atcmd/CMakeLists.txt` 会编入本目录源文件并
> 注入 `-DCLAW_AGENT_AUTO_TEST=1`;关闭时连命令分发都被 `#if` 去掉。TESTER 与 DUT
> **共用这份开启过开关的 image**。

---

## 1. 硬件台架(GPIO)

| 角色 | board_info.json5 alias | 串口 | 说明 |
|---|---|---|---|
| 被测板 DUT | `RTL8721F_COM23_DUT` | COM23 | 跑 ameba-claw,按键 active-low + 内部上拉 |
| 测试板 TESTER | `RTL8721F_COM24_TESTER` | COM24 | 同型号、**同一份 image**,负责注入 |

DUT 按键(`boards/EV721FL0_R03/board.json`)

**接线规则(必须遵守):**
1. **GPIO 一一同名对应**:TESTER.PA_22 ↔ DUT.PA_22(btn_a),其余同名相连。
2. **两板共地**(GND 短接)。
3. 只拉低不上驱(固件已保证开漏模拟,TESTER 绝不输出高电平)。

---

## 2. AT 命令清单(发给 TESTER 的串口)

```
AT+CLAW=gpio_ctrl,<pin>,click           单击(默认 40ms)
AT+CLAW=gpio_ctrl,<pin>,double          双击
AT+CLAW=gpio_ctrl,<pin>,long[,<ms>]     长按(默认 800ms)
AT+CLAW=gpio_ctrl,<pin>,bounce          抖动(6 次弹跳后稳定按住,测去抖)
AT+CLAW=gpio_ctrl,<pin>,press           按住不放(保持拉低)
AT+CLAW=gpio_ctrl,<pin>,release         释放(回高阻)
AT+CLAW=gpio_ctrl,<pin>,seq,d0,d1,...   自定义毫秒时序(交替按/放,自动以释放收尾)
```
成功返回:`+CLAW:gpio_ctrl,<pin>,<gesture>,done` 后跟 `OK`。
时序在设备端精确执行(实测 long,300≈350ms、seq 总和≈各段之和)。

可调默认值:`include/ameba_claw_defs.h` 的 `CLAW_GPIOCTRL_*`
(CLICK_MS / LONG_MS / DOUBLE_GAP_MS / BOUNCE_EDGE_MS / BOUNCE_COUNT / SEQ_MAX / MAX_MS);
编译开关 `CLAW_AGENT_AUTO_TEST` 同在该头文件(默认 0)。

---

## 3. 标准端到端流程(按键检测任务)

> ⚠️ CLAUDE.md 约束:被测的"按键检测脚本"必须让 **ameba-claw 的 LLM 自己**
> 探索能力去写 Lua。本流程**只**通过 TESTER 物理"按键",绝不替 DUT 写检测代码。

1. **派任务给 DUT**(与具体实现无关的自然语言):
   `AT+CLAW=ask,<让它做一个检测按键A并报告的功能>` —— 发到 `RTL8721F_COM23_DUT`。
   观察 harness:它是否自己去查 cap / skill / board 文档、自己写出 Lua 技能。
2. **等 DUT 就绪**后,从 TESTER 注入激励:
   `AT+CLAW=gpio_ctrl,PA_22,click` —— 发到 `RTL8721F_COM24_TESTER`。
3. **三路交叉核对**:
   - DUT 串口:是否打出按键事件 / 任务是否响应。
   - VQA(`auto_perf/vqa.sh`):拍 DUT 屏,UI 是否变化。
   - ccglass:必要时看 LLM 请求真相。
4. **覆盖各手势**:单击 / 双击 / 长按 / 抖动(去抖)/ 自定义 seq,逐项验证
   DUT 的检测逻辑是否区分正确;尤其 `bounce` 用来暴露未去抖的误触发。

## 4. 冒烟自检(不接线也能跑)

只验证命令解析与时序执行(引脚悬空,不看 DUT 反应):
```
AT+CLAW=gpio_ctrl,PA_22,click     -> ...,click,done / OK
AT+CLAW=gpio_ctrl,PA_22,long,300  -> ...,long,done  (约 350ms)
AT+CLAW=gpio_ctrl,PZ_9,click      -> bad pin ... / ERROR
AT+CLAW=gpio_ctrl,PA_22,triple    -> unknown gesture ... / ERROR
```
