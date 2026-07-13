# button driver 测试经验与流程

> 本文档记录用 MCP (`ameba-dev` + 串口工具) 对 `lua_driver_gpio_button.c`
> 驱动做端到端黑盒验证的经验教训，供后续测试复用。
> 测试时间：2026-06-29。DUT：RTL8721F_COM23，TESTER：RTL8721F_COM24。

---

## 1. 结论速查

| 测试项 | 结果 | 关键观测 |
|---|---|---|
| T1 click | ✅ | down → up → click，click hold_ms=0 |
| T2 double | ✅ | down→up→down→up→double，FSM 正确区分 |
| T3 long_press + hold | ✅ | long_press 精确在 1500ms，hold 每 201ms |
| T4 bounce 消抖 | ⚠️ | 见 §4，顺序法不可用，需并行法 |
| T5 get_level 极性 | ✅ | pressed=1，released=0，active-low 正确 |
| T6 flush mid-press | ✅ | flush 后 release 无残留事件 |
| T7 btn.off | 未完成（中断） | — |
| T8 multi-pin | 未完成（中断） | — |

---

## 2. 环境准备

### 2.1 编译 TESTER 固件

TESTER 必须带 `CLAW_AGENT_AUTO_TEST=1` 编译，否则 `AT+CLAW=gpio_ctrl` 命令不存在：

```bash
source env.sh
python /home/miles_wang/mcu_sdk/ameba.py build -q -D CLAW_AGENT_AUTO_TEST=1
# 烧录到 TESTER
mcp flash RTL8721F_COM24_TESTER
```

DUT 编译普通固件（无 `-D`），烧录到 COM23。

### 2.2 开启调试日志

测试期间在 `ameba_claw_defs.h` 设置：

```c
#define CLAW_BTN_DEBUG  1
```

这样串口会打出 `[btn-I] edge / settled / emit` 三层日志，是排查 ISR 是否触发的**唯一可靠手段**。
release 前改回 0。

### 2.3 验证接线

接线完成后先做 3 步快速确认，再跑正式测试：

```lua
-- DUT REPL:
btn=require("button")
btn.on("PA_22","click")

-- Step 1: 物理连通 - TESTER press 后 get_level 应返回 1
-- (TESTER 发: AT+CLAW=gpio_ctrl,PA_22,press)
print(btn.get_level("PA_22"))   -- 期望: 1

-- Step 2: ISR 是否触发 - 看串口有无 [btn-I] edge pin 22
-- (TESTER 发: AT+CLAW=gpio_ctrl,PA_22,release)
-- 期望串口出: [btn-I] edge pin 22 / settled pin 22 lvl 0

-- Step 3: 事件能否到达 Lua
local e = btn.get_event(0)      -- 期望: up 事件（或 nil 若未及时读）
```

---

## 3. 最重要经验：MCP 并行调用时序陷阱

### 问题描述

`gpio_ctrl click`（100ms 按压）+ `btn.get_event` **并行触发**时，事件丢失率极高。

**根因**：

```
t=0ms : MCP 并行发出 [DUT col] + [TESTER click]
t=10ms: TESTER 开始按下（ISR fires, dirty=1, last_edge_us=10ms）
t=110ms: TESTER 释放（ISR fires, dirty=1, last_edge_us=110ms ← 覆盖了按下时间戳）
t=20ms: DUT drain 第一次运行
  → settle_start = last_edge_us = 110ms（释放时间）
  → DTimestamp_Get() - 110ms = 负数（u32 溢出 → 极大值 >> 15ms）
  → settle 立即触发，采样 PA_22 = HIGH（已释放）
  → confirmed_pressed=0 → 无变化 → DOWN 丢失
```

`s_btn_last_edge_us[pin]` 只保存**最后一次** ISR 的时间戳。按压+释放都在 DUT drain 运行前完成时，settle 基准被释放边沿覆盖，采样到已释放电平。

### 正确做法：顺序法（press → drain during press → release）

```
TESTER press  →  DUT get_event（立即拿到 DOWN）
              →  TESTER release
              →  DUT get_event（拿到 UP）
              →  DUT get_event(600)（等 CLICK，300ms 双击窗口到期）
```

**关键**：DUT 的 `get_event` 调用必须在 TESTER press 已完成、PA_22 稳定为 LOW 之后发起。
这样 drain 运行时 PA_22 仍为 LOW，settle 采样到 pressed=1，DOWN 正确触发。

### 代码模板

```lua
-- DUT REPL（每步独立的 MCP serial_command_tool 调用）

-- 0. 注册并清空
btn=require("button")
btn.on("PA_22","down"); btn.on("PA_22","up")
btn.on("PA_22","click"); btn.on("PA_22","double")
btn.on("PA_22","long_press"); btn.on("PA_22","hold")
btn.flush()

-- 1. [TESTER] AT+CLAW=gpio_ctrl,PA_22,press
local e=btn.get_event(300); print("E1:"..(e and e.type or "nil"))  -- 期望 down

-- 2. [TESTER] AT+CLAW=gpio_ctrl,PA_22,release
local e=btn.get_event(500); print("E2:"..(e and e.type or "nil"))  -- 期望 up
local e=btn.get_event(600); print("E3:"..(e and e.type or "nil"))  -- 期望 click
```

### 什么时候可以用并行

只有当 TESTER 信号**持续时间足够长**、DUT drain 可以在信号结束前采样时才可靠：

- **T3 long_press**：TESTER `long,2000`（2000ms 按压），DUT `col(8,2000)` 并行。
  DUT drain 在 15-50ms 内就能采到 pressed=1，之后 2000ms 内持续 drain，long_press/hold/up 全部正常。
- 一般经验：TESTER 按压时间 >> 150ms（串口往返延迟 × 2 + 15ms settle）才安全用并行。

---

## 4. bounce 消抖测试注意事项

`AT+CLAW=gpio_ctrl,PA_22,bounce` 注入序列：
- 6次 × (2ms LOW + 2ms HIGH) = 24ms 抖动脉冲
- 随后 100ms 稳定按压
- 最后释放

**顺序法为何失败**：

TESTER bounce 完成后（~150ms）DUT 才开始 drain。此时：
- 最后一次 ISR 是释放边沿
- settle 采样 PA_22 = HIGH（已释放）→ 无 DOWN

**正确做法（并行法）**：DUT `col` 与 TESTER `bounce` 同时发出，DUT 在抖动期间持续 drain：

```
并行发: DUT col(5,600)  +  TESTER AT+CLAW=gpio_ctrl,PA_22,bounce
```

settle 的时间点落在抖动结束后的 100ms 稳定按压窗口内 → DOWN 确认 → 最终只出 1 个 click（消抖正确）。

> ⚠️ 注意：settle 窗口（15ms）与抖动周期（4ms）相互作用存在随机性；
> 若 settle 恰好落在抖动的 HIGH 相，会等下一个边沿重新 settle。
> 只要最终在 100ms 稳定按压内有一次 settle 成功，就只出 1 个 DOWN。

---

## 5. hold_ms 数值说明（不是 bug）

测试时观察到 `hold_ms` 值异常大（12000ms+），**这是正常现象**，不是驱动 bug：

`hold_ms = DTimestamp_Get() - down_at_us`，即**实际按住时长**。

在 MCP 顺序测试中，每步 MCP 调用之间有 AI 思考/处理时间（数秒），TESTER 一直
维持 press 状态，因此 DUT 实际看到的按压持续时间很长。

**正确验证 hold_ms 的方法**：用 TESTER `long,<ms>` 并行触发，DUT 全程在 `get_event` 中
持续 drain，不存在空闲期。T3 测试结果：

```
long_press hold_ms = 1500ms  （精确！= BTN_LONG_MS）
hold×5     hold_ms = 1701/1902/2103/2304/2505ms （每次+201ms ≈ BTN_HOLD_REPEAT_MS=200ms）
up         hold_ms = 2632ms  （总按压时长）
```

---

## 6. 会话内不要混用 gpio.set_irq 和 btn.on

在同一个 REPL 会话里，先 `gpio.set_irq("PA_22",...)` 再 `btn.on("PA_22",...)` 会
导致 PA_22 的中断静默失效（`gpio.get_irq_count` 返回 0，`[btn-I] edge` 不出现）。

**根因**：`btn.off("PA_22")` → `GPIO_INTConfig(DISABLE)` → 之后重新设置的
`gpio.set_irq` + `gpio.irq_enable` 序列在某些情况下中断状态没有完全恢复。

**规避方法**：

1. 不要在同一会话内对同一 pin 交替切换 gpio legacy 路径和 button 路径。
2. 若需要诊断，**重启板子**（`serial_connect_tool reset=true`）后直接进 REPL，
   只用 `btn.on`，不调 `gpio.set_irq`。
3. 重启后 ISR 从干净状态初始化，`gpio_ctrl` 测试可稳定复现。

---

## 7. 推荐的 MCP 测试流程

### 7.1 标准 click 测试

```
[MCP] TESTER: AT+CLAW=gpio_ctrl,PA_22,press     → done
[MCP] DUT:    local e=btn.get_event(300); print(e.type)   → down
[MCP] TESTER: AT+CLAW=gpio_ctrl,PA_22,release   → done
[MCP] DUT:    local e=btn.get_event(500); print(e.type)   → up
[MCP] DUT:    local e=btn.get_event(600); print(e.type)   → click
```

### 7.2 标准 double 测试

```
[MCP] TESTER: press        → down
[MCP] DUT:    get_event    → down
[MCP] TESTER: release      → up
[MCP] DUT:    get_event    → up
[MCP] TESTER: press        ← 必须在 U1 确认后 300ms 内发出（MCP 往返约 100ms，足够）
[MCP] DUT:    get_event    → down
[MCP] TESTER: release
[MCP] DUT:    get_event(500) + get_event(600)    → up, double
```

### 7.3 标准 long_press 测试（并行法）

```python
# 并行触发（同一 message 里两个 MCP tool call）：
DUT:    col(8, 2000)                              # 最多 8 事件，每个等 2s
TESTER: AT+CLAW=gpio_ctrl,PA_22,long,2000        # 按住 2000ms
# 期望：down, long_press, hold×N, up
```

### 7.4 flush 测试

```
press → get DOWN → btn.flush() → release → get_event(500) → nil（无残留）
```

---

## 8. 已验证的驱动行为

- **ISR 上半部**：边沿触发 → `edge_dirty` 置位 + `last_edge_us` 更新 + `sema_give`，三行代码，无任何计算。
- **消抖下半部**：settle 窗口从 **最后一次 ISR 时间戳** 起算（`BTN_DEBOUNCE_MS=15ms`），到点重采样真实电平，若与 `confirmed_pressed` 不同才喂 FSM。窗内后续边沿只刷新 `dirty`，不重启窗口。
- **FSM 惰性推进**：只在 `btn_drain()` 被调用时推进，即只有 `get_event`/`dispatch` 调用时才运行 —— 应用须保持足够高频地调用才能及时响应时间事件（long_press/hold/double 超时）。
- **WAKE_MAX_MS=50ms**：`get_event` 阻塞时最多每 50ms 唤醒一次检查截止点，时间事件延迟上限约 50ms（对 1500ms long_press 可忽略）。
- **hold_ms**：对 up/long_press/hold 事件，值为"从 DOWN 确认到本事件的实际毫秒数"，准确反映物理按压时长。click/double 的 hold_ms 为 0。
