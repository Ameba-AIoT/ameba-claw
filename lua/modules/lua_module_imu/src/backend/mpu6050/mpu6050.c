/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** mpu6050.c — InvenSense MPU-6050 (GY-521) backend for the Lua `imu` module.
**
** Talks to the sensor exclusively through the shared lua_driver_i2c C bus API
** (lua_i2c_bus_read_regs / lua_i2c_bus_write_regs), so it shares one controller
** lock with any Lua i2c code on the same bus — no separate mutex, no
** cross-module interleave.  Register map and default full-scale ranges
** (accel +/-16 g, gyro +/-2000 dps) are standard MPU-6050 values.
*/

#include <stdint.h>

#include "backend/mpu6050/mpu6050.h"
#include "lua_driver_i2c.h"

/* ── MPU-6050 register map ───────────────────────────────────────────────── */
#define MPU6050_REG_SMPLRT_DIV       0x19
#define MPU6050_REG_CONFIG           0x1A
#define MPU6050_REG_GYRO_CONFIG      0x1B
#define MPU6050_REG_ACCEL_CONFIG     0x1C
#define MPU6050_REG_INT_ENABLE       0x38
#define MPU6050_REG_INT_STATUS       0x3A   /* burst start: status + data */
#define MPU6050_REG_ACCEL_XOUT_H     0x3B
#define MPU6050_REG_TEMP_OUT_H       0x41
#define MPU6050_REG_GYRO_XOUT_H      0x43
#define MPU6050_REG_PWR_MGMT_1       0x6B
#define MPU6050_REG_WHO_AM_I         0x75

#define MPU6050_CHIP_ID              0x68
#define MPU6050_DEFAULT_ADDR         0x68   /* AD0 = GND (0x69 when AD0 = VDDIO) */
#define MPU6050_DEFAULT_FREQ         400000 /* Hz — fast-mode I2C */
#define MPU6050_PWR_CLKSEL_PLL_XGYRO 0x01   /* wake + gyro-X PLL clock source */
#define MPU6050_DLPF_CFG_44HZ        0x03
#define MPU6050_GYRO_FS_2000DPS      0x18
#define MPU6050_ACCEL_FS_16G         0x18
#define MPU6050_INT_DATA_RDY_EN      0x01

/* Single-burst layout, starting at INT_STATUS (0x3A): 1 status byte, 6 accel,
 * 2 temp, 6 gyro = 15 bytes, all captured at the same instant. */
#define MPU6050_BURST_LEN            15
#define MPU6050_BURST_STATUS_OFF     0
#define MPU6050_BURST_ACCEL_OFF      1
#define MPU6050_BURST_TEMP_OFF       7
#define MPU6050_BURST_GYRO_OFF       9

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Combine two big-endian bytes into a signed 16-bit value. */
static int16_t mpu6050_be16(const uint8_t *p)
{
    return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int mpu6050_write_reg(const imu_bus_t *bus, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return (lua_i2c_bus_write_regs(bus->i2c_idx, bus->addr, buf, 2) == LUA_I2C_OK)
           ? 0 : -1;
}

static int mpu6050_read_regs(const imu_bus_t *bus, uint8_t reg,
                             uint8_t *buf, uint32_t len)
{
    return (lua_i2c_bus_read_regs(bus->i2c_idx, bus->addr, reg, buf, len)
            == LUA_I2C_OK) ? 0 : -1;
}

/* ── Backend vtable implementation ───────────────────────────────────────── */

static int mpu6050_who_am_i(const imu_bus_t *bus, uint8_t *id)
{
    return mpu6050_read_regs(bus, MPU6050_REG_WHO_AM_I, id, 1);
}

static int mpu6050_probe_init(const imu_bus_t *bus)
{
    uint8_t chip_id = 0;
    if (mpu6050_read_regs(bus, MPU6050_REG_WHO_AM_I, &chip_id, 1) != 0) {
        return -1;
    }
    if (chip_id != MPU6050_CHIP_ID) {
        return -1;  /* not an MPU-6050 at this address */
    }

    /* Wake from sleep + select the gyro-X PLL clock, then set the sample rate,
     * DLPF, full-scale ranges and enable the data-ready interrupt flag. */
    if (mpu6050_write_reg(bus, MPU6050_REG_PWR_MGMT_1,
                          MPU6050_PWR_CLKSEL_PLL_XGYRO) != 0 ||
        mpu6050_write_reg(bus, MPU6050_REG_SMPLRT_DIV, 0x00) != 0 ||
        mpu6050_write_reg(bus, MPU6050_REG_CONFIG, MPU6050_DLPF_CFG_44HZ) != 0 ||
        mpu6050_write_reg(bus, MPU6050_REG_GYRO_CONFIG,
                          MPU6050_GYRO_FS_2000DPS) != 0 ||
        mpu6050_write_reg(bus, MPU6050_REG_ACCEL_CONFIG,
                          MPU6050_ACCEL_FS_16G) != 0 ||
        mpu6050_write_reg(bus, MPU6050_REG_INT_ENABLE,
                          MPU6050_INT_DATA_RDY_EN) != 0) {
        return -1;
    }
    return 0;
}

static int mpu6050_read_sample(const imu_bus_t *bus, imu_sample_t *out)
{
    uint8_t raw[MPU6050_BURST_LEN];
    if (mpu6050_read_regs(bus, MPU6050_REG_INT_STATUS, raw, sizeof(raw)) != 0) {
        return -1;
    }

    out->int_status = raw[MPU6050_BURST_STATUS_OFF];
    out->accel.x    = mpu6050_be16(&raw[MPU6050_BURST_ACCEL_OFF + 0]);
    out->accel.y    = mpu6050_be16(&raw[MPU6050_BURST_ACCEL_OFF + 2]);
    out->accel.z    = mpu6050_be16(&raw[MPU6050_BURST_ACCEL_OFF + 4]);
    out->temp       = mpu6050_be16(&raw[MPU6050_BURST_TEMP_OFF]);
    out->gyro.x     = mpu6050_be16(&raw[MPU6050_BURST_GYRO_OFF + 0]);
    out->gyro.y     = mpu6050_be16(&raw[MPU6050_BURST_GYRO_OFF + 2]);
    out->gyro.z     = mpu6050_be16(&raw[MPU6050_BURST_GYRO_OFF + 4]);
    return 0;
}

static int mpu6050_read_temp_raw(const imu_bus_t *bus, int16_t *raw_out)
{
    uint8_t raw[2];
    if (mpu6050_read_regs(bus, MPU6050_REG_TEMP_OUT_H, raw, sizeof(raw)) != 0) {
        return -1;
    }
    *raw_out = mpu6050_be16(raw);
    return 0;
}

static int mpu6050_read_int_status(const imu_bus_t *bus, uint8_t *status)
{
    return mpu6050_read_regs(bus, MPU6050_REG_INT_STATUS, status, 1);
}

/* MPU-6050 datasheet: temp_C = raw / 340 + 36.53 */
static double mpu6050_temp_to_celsius(int16_t raw)
{
    return (double)raw / 340.0 + 36.53;
}

const imu_backend_t imu_backend_mpu6050 = {
    .name            = "mpu6050",
    .default_addr    = MPU6050_DEFAULT_ADDR,
    .default_freq    = MPU6050_DEFAULT_FREQ,
    .probe_init      = mpu6050_probe_init,
    .read_sample     = mpu6050_read_sample,
    .read_temp_raw   = mpu6050_read_temp_raw,
    .read_int_status = mpu6050_read_int_status,
    .who_am_i        = mpu6050_who_am_i,
    .temp_to_celsius = mpu6050_temp_to_celsius,
};
