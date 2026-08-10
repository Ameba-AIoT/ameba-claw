/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** bmi270.c — Bosch BMI270 6-axis IMU backend for the Lua `imu` module.
**
** Talks to the sensor through the shared lua_driver_i2c C bus API
** (lua_i2c_bus_read_regs / lua_i2c_bus_write_regs).  All transactions hold
** the per-controller lock, so this backend and any Lua i2c code on the same
** bus can never interleave.
**
** Init sequence:
**   1. Soft-reset (CMD=0xB6), wait 10 ms.
**   2. Write feature-engine config file (8 KB) in 16-byte chunks via INIT_DATA.
**      INIT_ADDR bit layout: INIT_ADDR_0[3:0]=word_addr[3:0] (upper nibble
**      reserved=0), INIT_ADDR_1[7:0]=word_addr[11:4].
**   3. Set INIT_CTRL=0x01, poll INTERNAL_STATUS[3:0]==0x01 (≤200 ms).
**   4. Disable advanced-power-save, configure ACC+GYR ODR/range/BWP.
**   5. Enable ACC+GYR via PWR_CTRL, wait 40 ms for sensor startup.
**
** Data read: single 24-byte burst from ACC_X_LSB (0x0C) through
** TEMPERATURE_1 (0x23), covering gyro + accel + INT_STATUS + temperature.
** All data is little-endian (unlike MPU-6050 which is big-endian).
**
** Temperature: T_celsius = 23.0 + raw_int16 / 512.0
** Accel FS ±16 g → 2048 LSB/g; Gyro FS ±2000 dps → 16.384 LSB/dps.
*/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "backend/bmi270/bmi270.h"
#include "backend/bmi270/bmi270_config.h"
#include "ameba_soc.h"
#include "lua_driver_i2c.h"
#include "os_wrapper_time.h"

/* ── Register map ─────────────────────────────────────────────────────────── */
#define BMI270_REG_CHIP_ID          0x00u  /* Expected value: 0x24 */
#define BMI270_REG_ERR_REG          0x02u
#define BMI270_REG_STATUS           0x03u  /* bit4=drdy_acc, bit3=drdy_gyr */
#define BMI270_REG_ACC_X_LSB        0x0Cu  /* burst start: acc(6)+gyr(6) */
#define BMI270_REG_GYR_X_LSB        0x12u
#define BMI270_REG_INT_STATUS_0     0x1Cu
#define BMI270_REG_INT_STATUS_1     0x1Du
#define BMI270_REG_INTERNAL_STATUS  0x21u  /* bits[3:0]: 0x01 = init_ok */
#define BMI270_REG_TEMPERATURE_0    0x22u  /* LSB, little-endian int16 */
#define BMI270_REG_TEMPERATURE_1    0x23u  /* MSB */
#define BMI270_REG_ACC_CONF         0x40u  /* odr[3:0] | bwp[6:4] | fperf[7] */
#define BMI270_REG_ACC_RANGE        0x41u
#define BMI270_REG_GYR_CONF         0x42u  /* odr[3:0] | bwp[5:4] | nperf[6] | fperf[7] */
#define BMI270_REG_GYR_RANGE        0x43u
#define BMI270_REG_INIT_CTRL        0x59u
#define BMI270_REG_INIT_ADDR_0      0x5Bu  /* word_addr[3:0]; bits[7:4] reserved */
#define BMI270_REG_INIT_ADDR_1      0x5Cu  /* word_addr[11:4] */
#define BMI270_REG_INIT_DATA        0x5Eu  /* config file write port */
#define BMI270_REG_PWR_CONF         0x7Cu  /* bit0=adv_power_save */
#define BMI270_REG_PWR_CTRL         0x7Du  /* bit2=acc_en, bit1=gyr_en */
#define BMI270_REG_CMD              0x7Eu

/* ── Register values ──────────────────────────────────────────────────────── */
#define BMI270_CHIP_ID              0x24u
#define BMI270_CMD_SOFT_RESET       0xB6u
#define BMI270_DEFAULT_ADDR         0x68u  /* SDO = GND */
#define BMI270_DEFAULT_FREQ         400000u

/* ACC_CONF: ODR=200Hz(9) | BWP=Normal(2<<4) | perf=1(1<<7) = 0xA9 */
#define BMI270_ACC_CONF_VAL         0xA9u
/* ACC_RANGE: ±16 g = 0x03 */
#define BMI270_ACC_RANGE_VAL        0x03u
/* GYR_CONF: ODR=200Hz(9) | BWP=Normal(2<<4) | noise_perf(1<<6) | filt_perf(1<<7) = 0xE9 */
#define BMI270_GYR_CONF_VAL         0xE9u
/* GYR_RANGE: ±2000 dps = 0x00 */
#define BMI270_GYR_RANGE_VAL        0x00u
/* PWR_CTRL: acc_en(bit2) | gyr_en(bit1) = 0x06 */
#define BMI270_PWR_CTRL_ACC_GYR     0x06u
/* PWR_CONF: APS disabled = 0x00 */
#define BMI270_PWR_CONF_ACTIVE      0x00u

/* Config file loading */
#define BMI270_CONFIG_CHUNK         16u    /* bytes per INIT_DATA write */
#define BMI270_INIT_STATUS_OK       0x01u  /* INTERNAL_STATUS[3:0] target */
#define BMI270_INIT_TIMEOUT_MS      200u   /* poll timeout after INIT_CTRL=1 */

/* Burst read from GYR_X_LSB (0x0C) to TEMPERATURE_1 (0x23): 24 bytes.
 * Layout: acc(0-5) gyr(6-11) stime(12-14) rsv(15) istat(16-17)
 *         sc_out(18-19) wr_gest(20) int_status(21) temp(22-23). */
#define BMI270_BURST_START          BMI270_REG_ACC_X_LSB
#define BMI270_BURST_LEN            24u
#define BMI270_BURST_ACC_OFF        0
#define BMI270_BURST_GYR_OFF        6
#define BMI270_BURST_INT_STATUS_OFF 16
#define BMI270_BURST_TEMP_OFF       22

/* ── Low-level helpers ────────────────────────────────────────────────────── */

static int bmi270_write_reg(const imu_bus_t *bus, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return (lua_i2c_bus_write_regs(bus->i2c_idx, bus->addr, buf, 2) == LUA_I2C_OK)
           ? 0 : -1;
}

static int bmi270_read_regs(const imu_bus_t *bus, uint8_t reg,
                            uint8_t *buf, uint32_t len)
{
    return (lua_i2c_bus_read_regs(bus->i2c_idx, bus->addr, reg, buf, len)
            == LUA_I2C_OK) ? 0 : -1;
}

/* Combine two little-endian bytes into a signed 16-bit value. */
static int16_t bmi270_le16(const uint8_t *p)
{
    return (int16_t)(((uint16_t)p[1] << 8) | p[0]);
}

/* ── Config file load ─────────────────────────────────────────────────────── */

static int bmi270_load_config(const imu_bus_t *bus)
{
    /* Disable advanced power save during config load. */
    if (bmi270_write_reg(bus, BMI270_REG_PWR_CONF, BMI270_PWR_CONF_ACTIVE) != 0) {
        return -1;
    }
    /* Bosch SDK requires ≥450 μs after APS disable before touching INIT_CTRL. */
    rtos_time_delay_ms(1);

    /* Start config download mode (INIT_CTRL = 0). */
    if (bmi270_write_reg(bus, BMI270_REG_INIT_CTRL, 0x00u) != 0) {
        return -1;
    }

    uint8_t data_buf[BMI270_CONFIG_CHUNK + 1];
    data_buf[0] = BMI270_REG_INIT_DATA;

    uint32_t num_chunks = BMI270_CONFIG_SIZE / BMI270_CONFIG_CHUNK;
    for (uint32_t i = 0; i < num_chunks; i++) {
        uint16_t word_addr = (uint16_t)(i * BMI270_CONFIG_CHUNK / 2u);
        uint8_t  addr_buf[3];
        /* Per Bosch bmi2.c upload_file(): INIT_ADDR_0 carries only word_addr[3:0]
         * (upper nibble reserved); INIT_ADDR_1 carries word_addr[11:4]. */
        addr_buf[0] = BMI270_REG_INIT_ADDR_0;
        addr_buf[1] = (uint8_t)(word_addr & 0x0Fu);
        addr_buf[2] = (uint8_t)((word_addr >> 4u) & 0xFFu);
        if (lua_i2c_bus_write_regs(bus->i2c_idx, bus->addr, addr_buf, 3) != LUA_I2C_OK) {
            return -1;
        }

        memcpy(&data_buf[1], &bmi270_config_file[i * BMI270_CONFIG_CHUNK],
               BMI270_CONFIG_CHUNK);
        if (lua_i2c_bus_write_regs(bus->i2c_idx, bus->addr, data_buf,
                                   (uint32_t)sizeof(data_buf)) != LUA_I2C_OK) {
            return -1;
        }
    }

    /* End config download mode (INIT_CTRL = 1). */
    if (bmi270_write_reg(bus, BMI270_REG_INIT_CTRL, 0x01u) != 0) {
        return -1;
    }

    /* Poll INTERNAL_STATUS[3:0] == 0x01 (init_ok), timeout ~200 ms. */
    uint8_t status = 0;
    uint32_t elapsed = 0;
    while (elapsed < BMI270_INIT_TIMEOUT_MS) {
        rtos_time_delay_ms(10);
        elapsed += 10;
        if (bmi270_read_regs(bus, BMI270_REG_INTERNAL_STATUS, &status, 1) != 0) {
            continue;
        }
        if ((status & 0x0Fu) == BMI270_INIT_STATUS_OK) {
            return 0;  /* success */
        }
    }
    RTK_LOGS(NOTAG, RTK_LOG_WARN, "[bmi270] INTERNAL_STATUS timeout, last val=%d\n", (int)status);
    return -1;  /* timed out waiting for init_ok */
}

/* ── Backend vtable implementation ───────────────────────────────────────── */

static int bmi270_probe_init(const imu_bus_t *bus)
{
    /* Verify chip identity. */
    uint8_t chip_id = 0;
    if (bmi270_read_regs(bus, BMI270_REG_CHIP_ID, &chip_id, 1) != 0) {
        return -1;
    }
    if (chip_id != BMI270_CHIP_ID) {
        return -1;
    }

    /* Soft reset, then wait for the chip to restart. */
    if (bmi270_write_reg(bus, BMI270_REG_CMD, BMI270_CMD_SOFT_RESET) != 0) {
        return -1;
    }
    rtos_time_delay_ms(10);

    /* Re-verify after reset. */
    chip_id = 0;
    if (bmi270_read_regs(bus, BMI270_REG_CHIP_ID, &chip_id, 1) != 0 ||
        chip_id != BMI270_CHIP_ID) {
        return -1;
    }

    /* Load the feature-engine config file. */
    if (bmi270_load_config(bus) != 0) {
        return -1;
    }

    /* Configure accelerometer: 200 Hz, Normal BWP, ±16 g. */
    if (bmi270_write_reg(bus, BMI270_REG_ACC_CONF,  BMI270_ACC_CONF_VAL)  != 0 ||
        bmi270_write_reg(bus, BMI270_REG_ACC_RANGE, BMI270_ACC_RANGE_VAL) != 0) {
        return -1;
    }

    /* Configure gyroscope: 200 Hz, Normal BWP, ±2000 dps. */
    if (bmi270_write_reg(bus, BMI270_REG_GYR_CONF,  BMI270_GYR_CONF_VAL)  != 0 ||
        bmi270_write_reg(bus, BMI270_REG_GYR_RANGE, BMI270_GYR_RANGE_VAL) != 0) {
        return -1;
    }

    /* Enable accelerometer and gyroscope. */
    if (bmi270_write_reg(bus, BMI270_REG_PWR_CTRL, BMI270_PWR_CTRL_ACC_GYR) != 0) {
        return -1;
    }

    /* Wait for sensors to settle. */
    rtos_time_delay_ms(40);
    return 0;
}

static int bmi270_read_sample(const imu_bus_t *bus, imu_sample_t *out)
{
    uint8_t raw[BMI270_BURST_LEN];
    if (bmi270_read_regs(bus, BMI270_BURST_START, raw, sizeof(raw)) != 0) {
        return -1;
    }

    out->gyro.x     = bmi270_le16(&raw[BMI270_BURST_GYR_OFF + 0]);
    out->gyro.y     = bmi270_le16(&raw[BMI270_BURST_GYR_OFF + 2]);
    out->gyro.z     = bmi270_le16(&raw[BMI270_BURST_GYR_OFF + 4]);
    out->accel.x    = bmi270_le16(&raw[BMI270_BURST_ACC_OFF + 0]);
    out->accel.y    = bmi270_le16(&raw[BMI270_BURST_ACC_OFF + 2]);
    out->accel.z    = bmi270_le16(&raw[BMI270_BURST_ACC_OFF + 4]);
    out->int_status = raw[BMI270_BURST_INT_STATUS_OFF];
    out->temp       = bmi270_le16(&raw[BMI270_BURST_TEMP_OFF]);
    return 0;
}

static int bmi270_read_temp_raw(const imu_bus_t *bus, int16_t *raw_out)
{
    uint8_t raw[2];
    if (bmi270_read_regs(bus, BMI270_REG_TEMPERATURE_0, raw, 2) != 0) {
        return -1;
    }
    *raw_out = bmi270_le16(raw);
    return 0;
}

static int bmi270_read_int_status(const imu_bus_t *bus, uint8_t *status)
{
    return bmi270_read_regs(bus, BMI270_REG_INT_STATUS_0, status, 1);
}

static int bmi270_who_am_i(const imu_bus_t *bus, uint8_t *id)
{
    return bmi270_read_regs(bus, BMI270_REG_CHIP_ID, id, 1);
}

/* T_celsius = 23.0 + raw / 512.0  (0x0000 = 23 °C, 1 LSB = 1/512 °C) */
static double bmi270_temp_to_celsius(int16_t raw)
{
    return 23.0 + (double)raw / 512.0;
}

const imu_backend_t imu_backend_bmi270 = {
    .name            = "bmi270",
    .default_addr    = BMI270_DEFAULT_ADDR,
    .default_freq    = BMI270_DEFAULT_FREQ,
    .probe_init      = bmi270_probe_init,
    .read_sample     = bmi270_read_sample,
    .read_temp_raw   = bmi270_read_temp_raw,
    .read_int_status = bmi270_read_int_status,
    .who_am_i        = bmi270_who_am_i,
    .temp_to_celsius = bmi270_temp_to_celsius,
};
