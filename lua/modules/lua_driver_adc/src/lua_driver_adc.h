/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LUA_DRIVER_ADC_H
#define LUA_DRIVER_ADC_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_adc(lua_State *L);
void lua_driver_adc_provision(void);

#ifdef __cplusplus
}
#endif

#endif /* LUA_DRIVER_ADC_H */
