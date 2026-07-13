# lua_driver_i2c

I2C driver for RTL8721F. Exposes a two-level **bus + device** master model plus an **I2C slave** interface.

Two controllers are available:

| Controller | Max frequency |
|------------|---------------|
| I2C0       | 400 kHz       |
| I2C1       | 3.4 MHz       |

Frequency is always specified in Hz. Speed mode is chosen automatically from the
frequency: `≤100 kHz` → standard (SS), `≤400 kHz` → fast (FS), `>400 kHz` → high
speed (HS).

## Pins

There are **no hardcoded default pins** — `sda` and `scl` are required arguments
to `i2c.new` / `i2c.new_slave`, and may be any pad that supports the selected
controller's I2C function (the driver applies the pinmux for you). The driver
also enables the pad's internal weak pull-up on SDA and SCL.

Pins used by the bundled tests, for reference:

| Test                    | Controller | SDA   | SCL   |
|-------------------------|------------|-------|-------|
| `test/test_i2c.lua`     | I2C0       | PB_15 | PB_16 |
| `test/i2c_sh1106.lua`   | I2C0       | PA_26 | PA_25 |
| `i2c,rw` / `i2c,slave`  | I2C0       | PA_26 | PA_25 |

---

## Master API

### `i2c.new(idx, sda, scl [, freq_hz])` → bus

Create and initialise an I2C master bus.

| Parameter | Type    | Description                                            |
|-----------|---------|--------------------------------------------------------|
| `idx`     | integer | Controller index: `0` or `1`                           |
| `sda`     | string  | SDA pin name, e.g. `"PB_15"`                           |
| `scl`     | string  | SCL pin name, e.g. `"PB_16"`                           |
| `freq_hz` | integer | Clock frequency in Hz (default `100000`, range 1–3400000) |

`idx 0` rejects frequencies above `400000`.

---

### `bus:scan()` → table

Probe all 7-bit addresses (1–127) and return a table of addresses that ACK'd.

```lua
local addrs = bus:scan()
for _, a in ipairs(addrs) do
    print(string.format("found 0x%02X", a))
end
```

---

### `bus:device(addr7)` → device

Bind a logical device to a 7-bit address. The bus remains open until explicitly closed.

```lua
local dev = bus:device(0x48)
```

---

### `bus:close()`

Mark the bus handle closed and reject further use of it.

**Does not disable the I2C peripheral clock.** The same physical controller may
still be in use by another concurrent `lua_run` execution, so the clock is left
on permanently once the controller has been opened (see *Concurrency &
resources* below). `close()` is therefore safe to call any number of times and
does not free hardware — it only invalidates this Lua handle.

---

### `dev:read_byte([mem_addr])` → integer

Read one byte from the device.

- With `mem_addr`: write the register address then restart-read 1 byte.
- Without `mem_addr`: plain 1-byte read.

```lua
local val = dev:read_byte(0x00)   -- read register 0x00
local raw = dev:read_byte()        -- raw read
```

---

### `dev:read(len [, mem_addr])` → string

Read `len` bytes (1–4096). Returns a binary string.

- With `mem_addr`: write the register address then restart-read `len` bytes.
- Without `mem_addr`: plain read of `len` bytes.

```lua
local data = dev:read(6, 0x3B)    -- read 6 bytes from register 0x3B
```

---

### `dev:write_byte(value [, mem_addr])`

Write one byte to the device.

- With `mem_addr`: sends `[mem_addr, value]` in a single transaction.
- Without `mem_addr`: sends `[value]` only.

```lua
dev:write_byte(0x01, 0x1A)   -- write 0x01 to register 0x1A
dev:write_byte(0xAA)          -- raw write
```

---

### `dev:write(data [, mem_addr])`

Write a byte string or a table of integers to the device (payload up to 4096 bytes).

- With `mem_addr`: prepends `mem_addr` to the payload.
- Without `mem_addr`: sends payload directly.

```lua
dev:write("\x01\x02\x03", 0x20)      -- write 3 bytes at register 0x20
dev:write({0x01, 0x02, 0x03})         -- raw write via table
```

---

### `dev:address()` → integer

Return the 7-bit address this device is bound to.

```lua
print(dev:address())  -- e.g. 72 (0x48)
```

---

### `dev:close()`

Release the device object. Does not disable the bus.

---

## Slave API

### `i2c.new_slave(idx, sda, scl, addr7 [, freq_hz])` → slave

Create and initialise an I2C slave that responds at `addr7`.

| Parameter | Type    | Description                                            |
|-----------|---------|--------------------------------------------------------|
| `idx`     | integer | Controller index: `0` or `1`                           |
| `sda`     | string  | SDA pin name                                           |
| `scl`     | string  | SCL pin name                                           |
| `addr7`   | integer | 7-bit slave address this device answers to             |
| `freq_hz` | integer | Clock frequency in Hz (default `100000`, range 1–3400000) |

---

### `slave:read(len [, timeout_ms])` → string

Receive up to `len` bytes from the master. Returns a binary string (may be
shorter than `len` if a timeout elapses).

- With `timeout_ms`: bounded wait; returns what was received when the timeout fires.
- Without `timeout_ms`: blocking read.

```lua
local hdr = s:read(4, 60000)   -- wait up to 60 s for a 4-byte header
```

---

### `slave:write(data)` → integer

Send a byte string or table of integers back to the master. Returns the number
of bytes actually sent.

```lua
local sent = s:write({0x11, 0x22, 0x33})
```

---

### `slave:address()` → integer

Return the 7-bit address this slave answers to.

---

### `slave:close()`

Mark the slave handle closed. Like `bus:close()`, this **does not disable the
I2C peripheral clock** — it only invalidates this Lua handle. The controller
stays powered so a concurrent execution sharing the same controller is never
cut off mid-transaction.

---

## Example

```lua
-- Init I2C0 at 400 kHz
local bus = i2c.new(0, "PB_15", "PB_16", 400000)

-- Scan for devices
for _, a in ipairs(bus:scan()) do
    print(string.format("found 0x%02X", a))
end

-- Read 2 bytes from register 0x00 of device at 0x68 (e.g. MPU-6050)
local dev = bus:device(0x68)
local raw = dev:read(2, 0x00)
print(string.byte(raw, 1), string.byte(raw, 2))

dev:close()
bus:close()
```

## Notes

- `mem_addr` is an 8-bit internal register/memory address inside the target device. Omit it for devices that have no internal address (plain stream reads/writes).
- `bus:scan()` takes approximately 10 ms per address; a full scan (127 addresses) takes about 1.3 s.
- The driver enables the pad's internal weak pull-up on SDA and SCL. For fast modes or long/loaded buses, external pull-up resistors (typically 4.7 kΩ) are still recommended.
- For a higher-level device built on this driver, see the SH1106 OLED helper in `lib/oled_sh1106.lua` (`lib/oled_sh1106.md`).

## Concurrency & resources

The two physical I2C controllers are a **shared global resource**, while each
`i2c.new` / `i2c.new_slave` only hands back a lightweight Lua handle. Several
`lua_run` jobs (plus timer callbacks) can run concurrently — the same controller
may be driven by 2–3 execution flows at once — so the driver is built around
four rules (matching the bus-class template in the porting guide):

1. **One mutex per controller, held for the whole transaction.** Every wire
   operation (`new`, `scan`, `read*`, `write*`, slave `read`/`write`) takes the
   controller's mutex for its full duration, so two executions can never
   interleave on the same bus. The take uses a **bounded timeout** (5 s): a
   stuck controller surfaces as a Lua `error("controller busy")` instead of
   hanging every caller forever.
2. **Reference-counted init/deinit.** The *first* `new`/`new_slave` on a
   controller runs `I2C_Init`; each additional compatible handle just adds a
   reference. The configuration slot is released only when the **last** handle
   is garbage-collected. Hardware registers are never reset underneath an
   in-flight transfer.
3. **Conflicting config is rejected, not silently ignored.** While a controller
   is live, re-opening it with a different mode (master vs slave), frequency,
   pins, or slave address raises an `error()` — you cannot accidentally
   reconfigure a bus another job is using. Re-opening with the *same* config
   succeeds and shares the controller.
4. **`close()` / GC never powers the controller down.** `close()` only marks the
   Lua handle closed; the controller reference is dropped at GC. The peripheral
   clock stays on for the lifetime of the boot, so closing a handle in one job
   cannot starve another job mid-transaction.

**Critical-section purity:** all argument validation and buffer allocation
happen *before* the mutex is taken, and Lua results (tables, strings) are built
*after* it is released — nothing that can `longjmp` (`error`, allocation) ever
runs while the lock is held, which would otherwise skip the release and deadlock
the controller permanently.

**What this means for resource handling:** an `init → operation → deinit`
sequence (`new` → `read`/`write` → `close`) always succeeds and can be repeated;
the bundled `i2c,rw` test step 14 re-opens the bus after closing to prove the
handle is released cleanly. To switch a controller to a *different* mode or
frequency, let the previous handles go out of scope first (they are collected at
the end of each `lua_run` state) so the reference count returns to zero. The
only resource a script actually owns is the Lua handle; the hardware clock is
intentionally process-lifetime, not per-handle.
