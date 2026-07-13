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

/* The InvenSense MPU-6050 (GY-521) backend: 3-axis accel + 3-axis gyro + temp
 * over I2C, WHO_AM_I = 0x68.  Registered in imu_backend_find(). */
extern const imu_backend_t imu_backend_mpu6050;

#ifdef __cplusplus
}
#endif
