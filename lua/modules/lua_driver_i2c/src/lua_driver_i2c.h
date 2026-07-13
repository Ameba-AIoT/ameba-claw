/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int  luaopen_i2c(lua_State *L);
void lua_driver_i2c_init(void); /* call once at boot (single-threaded) */

/* ---- Shared C-level master-bus API -------------------------------------------
 * Lets other C modules (e.g. the IMU backends) drive an I2C controller through
 * the SAME per-controller lock + reference count + config slot as the Lua i2c
 * device API.  Because both paths share one mutex, a C module and Lua i2c code
 * can never interleave a transaction on the same controller (this is what fixes
 * the cross-module bus conflict).  Every call must run in a Lua task context,
 * never an ISR.  All functions return 0 on success, negative on error.
 *
 *   lua_i2c_bus_acquire(idx, sda, scl, freq);   // once, refcounted
 *   lua_i2c_bus_write_regs(idx, addr, buf, n);  // per transaction
 *   lua_i2c_bus_read_regs(idx, addr, reg, buf, n);
 *   lua_i2c_bus_release(idx);                    // once, at close/gc
 */
#define LUA_I2C_OK            0
#define LUA_I2C_ERR_ARG     (-1)   /* bad controller index / arguments */
#define LUA_I2C_ERR_BUSY    (-2)   /* lock acquire timed out */
#define LUA_I2C_ERR_CONFIG  (-3)   /* controller already open with other config */

/* Reference + configure controller idx (0/1) in master mode.  sda/scl are
 * PinName values passed as u8.  A compatible re-acquire just adds a reference;
 * an incompatible one returns LUA_I2C_ERR_CONFIG. */
int  lua_i2c_bus_acquire(int idx, uint8_t sda, uint8_t scl, uint32_t freq_hz);

/* Drop one reference taken by lua_i2c_bus_acquire (the clock stays on). */
void lua_i2c_bus_release(int idx);

/* Write len bytes to a 7-bit slave (buf usually starts with a register addr).
 * The whole transfer runs under the controller lock. */
int  lua_i2c_bus_write_regs(int idx, uint16_t addr, const uint8_t *buf, uint32_t len);

/* Set register pointer `reg` then repeated-START read len bytes into buf. */
int  lua_i2c_bus_read_regs(int idx, uint16_t addr, uint8_t reg, uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif
