/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Per-chip backend interface for lua_module_magnetometer.
 *
 * Each supported chip (BMM150 / ...) lives in its own subfolder under
 * src/backend/ and exports one `mag_backend_t` instance.  The Lua glue
 * is chip-agnostic and only talks to the chip through this vtable.
 */

#ifndef MAG_BACKEND_H
#define MAG_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* I2C bus descriptor shared with the lua_driver_i2c controller. */
typedef struct {
    int      i2c_idx;   /* 0 or 1 — shared lua_driver_i2c controller index */
    uint16_t addr;      /* 7-bit device address (may be updated by probe_alternates) */
} mag_bus_t;

/* One magnetometer reading returned to Lua. */
typedef struct {
    float   x;
    float   y;
    float   z;
    float   temperature; /* 0 when chip has no temperature output */
    uint8_t status;
} mag_sample_t;

/*
 * Backend vtable — one instance per chip.
 * All calls run in a Lua task context (never an ISR) and reach the I2C bus
 * through the shared lua_driver_i2c C bus API.  Return 0 on success, <0 on error.
 */
typedef struct mag_backend {
    const char *chip_name;
    size_t      state_size; /* bytes for the chip-private state block */

    /* Probe chip at bus->addr; apply default runtime config.
     * May update bus->addr if the initial address is wrong. */
    int  (*probe)(mag_bus_t *bus, void *state, uint8_t i2c_addr);

    /* Trigger one conversion and read x/y/z + status. */
    int  (*read_sample)(const mag_bus_t *bus, void *state, mag_sample_t *out);

    /* Read the chip interrupt/status register only. */
    int  (*read_status)(const mag_bus_t *bus, void *state, uint8_t *out);

    /* True if addr is one of the chip's valid I2C addresses. */
    bool (*is_supported_addr)(uint8_t addr);

    /* Default I2C address when caller omits it. */
    uint8_t (*default_addr)(void);

    /* Try alternate addresses after primary failed.  May be NULL. */
    int  (*probe_alternates)(mag_bus_t *bus, void *state, uint8_t primary);
} mag_backend_t;

/* BMM150 backend — defined in backend/bmm150/bmm150_backend.c */
extern const mag_backend_t mag_backend_bmm150;

/* Registry helpers. */
const mag_backend_t *mag_backend_find(const char *name);
const mag_backend_t *mag_backend_default(void);

#ifdef __cplusplus
}
#endif

#endif /* MAG_BACKEND_H */
