# lua_driver_pwm

PWM output driver for Ameba RTOS.
Parameters verified line-by-line against `src/lua_driver_pwm.c`.

---

## API reference

### `pwm.new(config)` → handle

| Field | Type | Required | Default | Validated at |
|-------|------|----------|---------|-------------|
| `pin` | string | **yes** | — | `luhw_check_pin` |
| `timer_idx` | integer | **yes** | — | must be 4–7 |
| `channel` | integer | **yes** | — | must be 0–3 |
| `frequency_hz` | integer | no | `1000` | `luaL_optinteger` |
| `duty_percent` | number | no | `50.0` | `luaL_optnumber`, 0–100 |

Opening the same timer twice at a *different* `frequency_hz` raises an error.

### Handle methods

| Method | Description |
|--------|-------------|
| `h:set_enabled(bool)` | `true` = run timer, `false` = stop output |
| `h:set_duty(percent)` | Set duty cycle 0.0–100.0 % |
| `h:set_duty(channel, percent)` | Channel arg accepted but ignored (always 1 ch/handle) |
| `h:set_frequency(hz)` | Change frequency; duty ratio is re-applied automatically |
| `h:get_channel_count()` | Returns `1` (one channel per handle) |
| `h:close()` | Release hardware; decrements timer refcnt |

---

## Hardware notes (RTL8721F)

PWM-capable timers: **TIM4–TIM7** (40 MHz clock, 16-bit counter).

| Timer | CH0 | CH1 | CH2 | CH3 |
|-------|-----|-----|-----|-----|
| TIM4 | PA_15 | PA_16 | PA_25 | **PA_26** |

TIM5–TIM7 available on other pins; see chip pinmux table.

Wiring for the default test:
- PA_26 → signal wire of SG90 (orange/yellow)
- GND → GND
- 5 V → VCC (SG90 can also run from 3.3 V signal)

---

## Test invocation

```
AT+CLAW=pwm            run full API smoke-test
AT+CLAW=servo          SG90 sweep, default range 45°→135°
AT+CLAW=servo,0,180,5,50   full sweep, 5° step, 50 ms/step
AT+CLAW=servo,at,90    move to 90°, hold 1 s
AT+CLAW=servo,at,0,2000    move to 0°, hold 2 s
```

---

## Test cases (AT+CLAW=pwm)

| Case | API under test | What is checked |
|------|---------------|-----------------|
| 1 | `pwm.new` | 1 kHz / 50 % — full param set |
| 2 | `set_enabled(true)` | Output starts |
| 3 | `set_duty` | Boundary values 0 / 10 / 25 / 50 / 75 / 90 / 100 % |
| 4 | `set_duty` | Continuous ramp 0→100→0 in 10 % steps |
| 5 | `set_frequency` | 500 / 1000 / 2000 / 5000 / 10000 Hz |
| 6 | `get_channel_count` | Returns 1 |
| 7 | `set_enabled(false/true)` | Disable / re-enable cycle, 50 ms hold |
| 8 | `set_enabled(false/true)` | Same cycle, 30 ms hold (additional timing) |
| 9 | `close` | Handle released cleanly |
| 10 | `pwm.new` error | `duty_percent=150` rejected |
| 11 | `pwm.new` error | `timer_idx=99` rejected |
| 12 | `pwm.new` error | Missing `pin` rejected |
| 13 | lifecycle | Re-open after close succeeds (init→deinit→init) |
| 14 | `pwm.new` servo | 50 Hz handle created |
| 15 | `set_duty` servo | Sweep 0°→180° in 10° steps |
| 16 | `set_duty` servo | Return sweep 180°→0° |
| 17 | `set_duty` servo | Hold at 0° / 90° / 180° (cardinal angles) |
| 18 | lifecycle servo | Close and re-open at 50 Hz |

---

## Concurrency & resources

### Init → operation → deinit lifecycle

```
pwm.new({...})          -- RCC enable + RTIM_TimeBaseInit (first handle on timer only)
h:set_enabled(true)     -- RTIM_Cmd(ENABLE)
h:set_duty(...)         -- RTIM_CCRxSet (per-channel, no lock)
h:set_frequency(...)    -- RTIM_PrescalerConfig + ChangePeriodImmediate (timer lock)
h:set_enabled(false)    -- RTIM_Cmd(DISABLE)
h:close()               -- refcnt--; RTIM_DeInit + RCC disable if refcnt == 0
```

Multiple handles on the same timer share one underlying `RTIM_TypeDef *`. The
driver uses a `rtos_mutex_t` per physical timer (created at boot in
`lua_driver_pwm_init()`) to serialise timer-wide operations.

### Lock scope

| Operation | Lock held? |
|-----------|-----------|
| `set_duty` | No — writes per-channel CCRx only |
| `set_enabled` | Yes — `RTIM_Cmd` is timer-wide |
| `set_frequency` | Yes — `RTIM_PrescalerConfig` + `ChangePeriodImmediate` |
| `new` (init path) | Yes — modifies `s_pwm_timer` shared state |
| `close` / `__gc` | Yes — decrements `refcnt`, conditionally deinits |

Lock timeout: 100 ms for all methods; 1000 ms for `close`/`__gc`.

### Verified by tests

Case 13 (and servo case 18) verify the **close → re-open** path: after `h:close()`, a new `pwm.new()` on the same timer succeeds, confirming that `RCC` and `RTIM` are correctly re-initialised after `refcnt` hits zero.
