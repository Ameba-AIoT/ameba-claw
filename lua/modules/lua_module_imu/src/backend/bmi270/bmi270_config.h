/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BMI270_CONFIG_SIZE 8192u

/* Bosch BMI270 feature-engine configuration image (BSD-3-Clause, Bosch Sensortec). */
extern const uint8_t bmi270_config_file[BMI270_CONFIG_SIZE];

#ifdef __cplusplus
}
#endif
