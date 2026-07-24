/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Backend registry for lua_module_magnetometer.
 * Add new chip backends here; nothing else needs to change.
 */

#include <stddef.h>
#include <string.h>

#include "mag_backend.h"

static const mag_backend_t *s_backends[] = {
    &mag_backend_bmm150,
};

#define N_BACKENDS (sizeof(s_backends) / sizeof(s_backends[0]))

const mag_backend_t *mag_backend_find(const char *name)
{
    if (!name || name[0] == '\0') {
        return s_backends[0];
    }
    for (size_t i = 0; i < N_BACKENDS; i++) {
        if (strcmp(s_backends[i]->chip_name, name) == 0) {
            return s_backends[i];
        }
    }
    return NULL;
}

const mag_backend_t *mag_backend_default(void)
{
    return s_backends[0];
}
