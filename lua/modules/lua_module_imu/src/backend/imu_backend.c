/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** imu_backend.c — IMU backend registry.
**
** One flat table of the compiled-in chip backends.  Add a new IMU by defining
** its imu_backend_t (see backend/mpu6050/mpu6050.c) and appending it here; the
** Lua glue and every other file stay unchanged.
*/

#include <string.h>

#include "backend/imu_backend.h"
#include "backend/mpu6050/mpu6050.h"

/* First entry is the default when new() omits `chip`. */
static const imu_backend_t *const s_backends[] = {
    &imu_backend_mpu6050,
};

#define IMU_BACKEND_COUNT (sizeof(s_backends) / sizeof(s_backends[0]))

const imu_backend_t *imu_backend_find(const char *name)
{
    if (name == NULL) {
        return imu_backend_default();
    }
    for (unsigned i = 0; i < IMU_BACKEND_COUNT; i++) {
        if (strcmp(s_backends[i]->name, name) == 0) {
            return s_backends[i];
        }
    }
    return NULL;
}

const imu_backend_t *imu_backend_default(void)
{
    return s_backends[0];
}
