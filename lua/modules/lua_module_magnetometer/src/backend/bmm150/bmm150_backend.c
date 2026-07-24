/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BMM150 backend for lua_module_magnetometer.
 *
 * Bridges the Bosch BMM150 SensorAPI (vendored, BSD-3) to the ameba_claw
 * lua_driver_i2c shared bus API.  Only ameba_claw driver types are used.
 *
 * Startup sequence (BMM150 quirk):
 *   1. Write POWER_CONTROL=0x01 to bring chip from suspend to sleep.
 *   2. Wait BMM150_START_UP_TIME (3 ms).
 *   3. bmm150_init() reads chip_id (must be 0x32) and trim registers.
 *   4. Configure FORCED mode + minimum repetitions (fastest conversion).
 *
 * Each read() call re-issues a FORCED mode trigger before reading data,
 * because FORCED mode auto-returns to sleep after each conversion.
 */

#include <string.h>
#include <stdint.h>

#include "ameba_soc.h"
#include "os_wrapper.h"

#include "bmm150.h"
#include "bmm150_defs.h"
#include "mag_backend.h"
#include "lua_driver_i2c.h"

/* ---- Internal state -------------------------------------------------------- */

typedef struct {
    struct bmm150_dev dev;
    mag_bus_t        *bus; /* pointer to caller-owned bus descriptor */
} bmm150_state_t;

/* ---- I2C delay helper ------------------------------------------------------ */

static void bmm150_delay_us_cb(uint32_t period_us, void *intf_ptr)
{
    (void)intf_ptr;
    /* Round up to whole milliseconds; rtos_time_delay_ms is the wrapper API. */
    uint32_t ms = (period_us + 999U) / 1000U;
    rtos_time_delay_ms(ms < 1U ? 1U : ms);
}

/* ---- I2C callbacks --------------------------------------------------------- */

static BMM150_INTF_RET_TYPE bmm150_i2c_read(uint8_t reg_addr, uint8_t *reg_data,
                                             uint32_t length, void *intf_ptr)
{
    bmm150_state_t *st = (bmm150_state_t *)intf_ptr;
    if (!st || !st->bus) {
        return BMM150_E_COM_FAIL;
    }
    int rc = lua_i2c_bus_read_regs(st->bus->i2c_idx, st->bus->addr,
                                   reg_addr, reg_data, length);
    return (rc == LUA_I2C_OK) ? BMM150_INTF_RET_SUCCESS : BMM150_E_COM_FAIL;
}

/*
 * BMM150 write: reg_data holds only the data bytes; we must prepend reg_addr
 * before calling lua_i2c_bus_write_regs (which expects [reg, data...]).
 * BMM150 never writes more than a handful of bytes at once — 8 bytes is enough.
 */
#define BMM150_WRITE_BUF_MAX 8

static BMM150_INTF_RET_TYPE bmm150_i2c_write(uint8_t reg_addr, const uint8_t *reg_data,
                                              uint32_t length, void *intf_ptr)
{
    bmm150_state_t *st = (bmm150_state_t *)intf_ptr;
    uint8_t buf[BMM150_WRITE_BUF_MAX + 1];
    if (!st || !st->bus) {
        return BMM150_E_COM_FAIL;
    }
    if (length > BMM150_WRITE_BUF_MAX) {
        return BMM150_E_COM_FAIL;
    }
    buf[0] = reg_addr;
    memcpy(&buf[1], reg_data, length);
    int rc = lua_i2c_bus_write_regs(st->bus->i2c_idx, st->bus->addr,
                                    buf, length + 1U);
    return (rc == LUA_I2C_OK) ? BMM150_INTF_RET_SUCCESS : BMM150_E_COM_FAIL;
}

/* ---- Init helpers ---------------------------------------------------------- */

static int bmm150_power_on(bmm150_state_t *st)
{
    uint8_t power = 0x01;
    int8_t rslt = bmm150_set_regs(BMM150_REG_POWER_CONTROL, &power, 1, &st->dev);
    if (rslt != BMM150_OK) {
        RTK_LOGS(NOTAG, RTK_LOG_WARN, "[mag_bmm150] power-on write failed: %d\n", (int)rslt);
        return -1;
    }
    /* BMM150_START_UP_TIME is 3000 us = 3 ms */
    bmm150_delay_us_cb(BMM150_START_UP_TIME, NULL);
    return 0;
}

static int apply_default_config(bmm150_state_t *st)
{
    struct bmm150_settings settings = { 0 };
    int8_t rslt;
    /* Minimum repetitions → fastest single conversion in FORCED mode. */
    uint8_t rep_xy = 0x00;
    uint8_t rep_z  = 0x00;
    rslt  = bmm150_set_regs(BMM150_REG_REP_XY, &rep_xy, 1, &st->dev);
    rslt |= bmm150_set_regs(BMM150_REG_REP_Z,  &rep_z,  1, &st->dev);
    if (rslt != BMM150_OK) {
        RTK_LOGS(NOTAG, RTK_LOG_WARN, "[mag_bmm150] set repetition failed: %d\n", (int)rslt);
        return -1;
    }
    settings.pwr_mode = BMM150_POWERMODE_FORCED;
    rslt = bmm150_set_op_mode(&settings, &st->dev);
    if (rslt != BMM150_OK) {
        RTK_LOGS(NOTAG, RTK_LOG_WARN, "[mag_bmm150] set FORCED mode failed: %d\n", (int)rslt);
        return -1;
    }
    RTK_LOGS(NOTAG, RTK_LOG_INFO, "[mag_bmm150] FORCED mode ok, chip_id=%d\n",
             (int)st->dev.chip_id);
    return 0;
}

/* ---- Backend vtable impl --------------------------------------------------- */

static int bmm150_probe(mag_bus_t *bus, void *state, uint8_t i2c_addr)
{
    bmm150_state_t *st = (bmm150_state_t *)state;
    int8_t rslt;

    bus->addr = i2c_addr;

    memset(&st->dev, 0, sizeof(st->dev));
    st->bus             = bus;
    st->dev.intf        = BMM150_I2C_INTF;
    st->dev.read        = bmm150_i2c_read;
    st->dev.write       = bmm150_i2c_write;
    st->dev.delay_us    = bmm150_delay_us_cb;
    st->dev.intf_ptr    = st;

    /* Bring chip from suspend to sleep; all registers read as 0 in suspend.
     * Return early on failure so probe_alternates only gets 1 TX_ABRT per
     * missing address instead of ~8 from the full init sequence. */
    if (bmm150_power_on(st) != 0) {
        return -1;
    }

    rslt = bmm150_init(&st->dev);
    RTK_LOGS(NOTAG, RTK_LOG_INFO, "[mag_bmm150] init addr=%d rslt=%d chip_id=%d\n",
             (int)i2c_addr, (int)rslt, (int)st->dev.chip_id);

    if (st->dev.chip_id != BMM150_CHIP_ID) {
        (void)bmm150_soft_reset(&st->dev);
        bmm150_delay_us_cb(BMM150_START_UP_TIME, NULL);
        rslt = bmm150_init(&st->dev);
        RTK_LOGS(NOTAG, RTK_LOG_INFO, "[mag_bmm150] re-init addr=%d rslt=%d chip_id=%d\n",
                 (int)i2c_addr, (int)rslt, (int)st->dev.chip_id);
        if (st->dev.chip_id != BMM150_CHIP_ID) {
            return -1;
        }
    }
    if (rslt != BMM150_OK) {
        return -1;
    }
    return apply_default_config(st);
}

static int bmm150_read_sample(const mag_bus_t *bus, void *state, mag_sample_t *out)
{
    bmm150_state_t *st = (bmm150_state_t *)state;
    struct bmm150_settings settings = { 0 };
    struct bmm150_mag_data data     = { 0 };
    int8_t rslt;

    (void)bus;

    /* FORCED mode auto-sleeps after each conversion; retrigger before reading.
     * After writing the FORCED opmode register, the chip needs ~8 ms to
     * complete the conversion before data registers 0x42-0x49 are valid.
     * Reading without this delay yields the previous cycle's stale data. */
    settings.pwr_mode = BMM150_POWERMODE_FORCED;
    rslt = bmm150_set_op_mode(&settings, &st->dev);
    if (rslt != BMM150_OK) {
        return -1;
    }
    st->dev.delay_us(8000, st->dev.intf_ptr); /* wait for conversion to complete */
    rslt = bmm150_read_mag_data(&data, &st->dev);
    if (rslt != BMM150_OK) {
        return -1;
    }
    out->x           = (float)data.x;
    out->y           = (float)data.y;
    out->z           = (float)data.z;
    out->temperature = 0.0f; /* BMM150 has no temperature output */
    rslt = bmm150_get_regs(BMM150_REG_INTERRUPT_STATUS, &out->status, 1, &st->dev);
    return (rslt == BMM150_OK) ? 0 : -1;
}

static int bmm150_read_status(const mag_bus_t *bus, void *state, uint8_t *out)
{
    bmm150_state_t *st = (bmm150_state_t *)state;
    (void)bus;
    int8_t rslt = bmm150_get_regs(BMM150_REG_INTERRUPT_STATUS, out, 1, &st->dev);
    return (rslt == BMM150_OK) ? 0 : -1;
}

static bool bmm150_is_supported_addr(uint8_t a)
{
    return a == BMM150_DEFAULT_I2C_ADDRESS              ||
           a == BMM150_I2C_ADDRESS_CSB_LOW_SDO_HIGH     ||
           a == BMM150_I2C_ADDRESS_CSB_HIGH_SDO_LOW     ||
           a == BMM150_I2C_ADDRESS_CSB_HIGH_SDO_HIGH;
}

static uint8_t bmm150_get_default_addr(void)
{
    return BMM150_DEFAULT_I2C_ADDRESS;
}

static int bmm150_probe_alternates(mag_bus_t *bus, void *state, uint8_t primary)
{
    static const uint8_t addrs[] = {
        BMM150_DEFAULT_I2C_ADDRESS,
        BMM150_I2C_ADDRESS_CSB_LOW_SDO_HIGH,
        BMM150_I2C_ADDRESS_CSB_HIGH_SDO_LOW,
        BMM150_I2C_ADDRESS_CSB_HIGH_SDO_HIGH,
    };
    for (size_t i = 0; i < sizeof(addrs); i++) {
        if (addrs[i] == primary) {
            continue;
        }
        RTK_LOGS(NOTAG, RTK_LOG_INFO, "[mag_bmm150] probe %d failed, retry %d\n",
                 (int)primary, (int)addrs[i]);
        if (bmm150_probe(bus, state, addrs[i]) == 0) {
            return 0;
        }
    }
    return -1;
}

const mag_backend_t mag_backend_bmm150 = {
    .chip_name        = "bmm150",
    .state_size       = sizeof(bmm150_state_t),
    .probe            = bmm150_probe,
    .read_sample      = bmm150_read_sample,
    .read_status      = bmm150_read_status,
    .is_supported_addr = bmm150_is_supported_addr,
    .default_addr     = bmm150_get_default_addr,
    .probe_alternates = bmm150_probe_alternates,
};
