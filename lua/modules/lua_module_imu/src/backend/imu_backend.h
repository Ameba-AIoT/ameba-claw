/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/*
** imu_backend.h — chip-agnostic IMU backend interface.
**
** The Lua `imu` module (lua_module_imu.c) is a thin dispatcher: it parses the
** new() options, acquires the shared I2C controller (lua_driver_i2c C bus API)
** and then talks to the sensor ONLY through the imu_backend_t vtable below.
** Adding a new IMU chip means adding one backend file that fills in this table
** and registering it in imu_backend_find(); no change to the Lua glue.
**
** Multi-backend layout: one Lua API, several chips.  Every
** backend reaches the bus with the shared lua_i2c_bus_read_regs/write_regs
** helpers, so it shares one lock with any Lua i2c user on the same controller.
*/

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which controller + slave address a backend should talk to.  Populated by the
 * Lua glue from the new() options (sda/scl/i2c/addr from board.json). */
typedef struct {
    int      i2c_idx;   /* 0 or 1 — shared lua_driver_i2c controller index */
    uint16_t addr;      /* 7-bit device address */
} imu_bus_t;

/* One 3-axis raw reading (signed 16-bit LSB, chip byte order already applied).
 * Three-axis raw reading, chip-agnostic. */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} imu_axes3_t;

/* One synchronised sample: status + accel + temp + gyro captured in a single
 * burst read so every field belongs to the same measurement instant. */
typedef struct {
    imu_axes3_t accel;       /* raw LSB */
    imu_axes3_t gyro;        /* raw LSB */
    int16_t     temp;        /* raw temperature register value */
    uint8_t     int_status;  /* INT_STATUS register (data-ready etc.) */
} imu_sample_t;

/* Backend vtable.  All calls run in a Lua task context (never an ISR) and use
 * the shared I2C bus helpers internally.  Return 0 on success, <0 on error. */
typedef struct imu_backend {
    const char *name;                                        /* e.g. "mpu6050" */

    /* Chip defaults applied by the Lua glue when new() omits these fields.
     * They live with the backend (not the chip-agnostic front end) so every
     * IMU carries its own address strap / preferred bus speed. */
    uint8_t     default_addr;   /* 7-bit slave address (e.g. 0x68 AD0=GND) */
    uint32_t    default_freq;   /* I2C clock in Hz (e.g. 400000 fast-mode) */

    /* Verify the chip is present (WHO_AM_I) and apply its default config.
     * Returns <0 (and touches nothing else) when this chip is not found. */
    int    (*probe_init)(const imu_bus_t *bus);

    /* One burst: status + accel + temp + gyro, all from the same instant. */
    int    (*read_sample)(const imu_bus_t *bus, imu_sample_t *out);

    /* Raw temperature register value (also present in read_sample). */
    int    (*read_temp_raw)(const imu_bus_t *bus, int16_t *raw);

    /* INT_STATUS register (data-ready / overflow flags). */
    int    (*read_int_status)(const imu_bus_t *bus, uint8_t *status);

    /* Identity register value (0x68 for a healthy MPU-6050). */
    int    (*who_am_i)(const imu_bus_t *bus, uint8_t *id);

    /* Chip-specific raw-temperature → Celsius conversion. */
    double (*temp_to_celsius)(int16_t raw);
} imu_backend_t;

/* Look up a backend by name (case-sensitive).  Returns NULL if unknown. */
const imu_backend_t *imu_backend_find(const char *name);

/* Default backend used when new() omits `chip` (first registered chip). */
const imu_backend_t *imu_backend_default(void);

#ifdef __cplusplus
}
#endif
