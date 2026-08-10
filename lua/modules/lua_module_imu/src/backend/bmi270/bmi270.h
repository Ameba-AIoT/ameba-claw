/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "backend/imu_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bosch BMI270 6-axis IMU backend (accel + gyro + temp, I2C).
 * WHO_AM_I = 0x24, default address 0x68 (SDO=GND). */
extern const imu_backend_t imu_backend_bmi270;

#ifdef __cplusplus
}
#endif
