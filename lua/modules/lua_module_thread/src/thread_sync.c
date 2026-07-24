/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * thread_sync.c — `thread.sync`: named, cross-job FreeRTOS synchronization
 * primitives (queue / counting semaphore / mutex-lock), using ameba's
 * os_wrapper (rtos_queue_t / rtos_sema_t / rtos_mutex_t) instead of raw
 * FreeRTOS, and ameba's cooperative-cancel convention (LUA_REGISTRYINDEX
 * ["__cancel_ptr"], same as lua_module_event.c::lua_event_wait).
 *
 * Objects live in a single global linked list guarded by one registry mutex.
 * A `waiter_count` on each object defends delete-while-blocked-on races:
 * delete refuses (returns "busy") while any call is inside acquire/release.
 */
#include "lua_module_thread_internal.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "os_wrapper.h"
#include "claw_compat.h"
#include "cap_lua.h"          /* cap_lua_set_quiescence_cb */
#include "ameba_claw_defs.h"

typedef enum {
    THREAD_SYNC_TYPE_QUEUE = 0,
    THREAD_SYNC_TYPE_SEM,
    THREAD_SYNC_TYPE_LOCK,
} thread_sync_type_t;

typedef struct thread_sync_object {
    char *name;
    thread_sync_type_t type;
    union {
        rtos_queue_t queue;
        rtos_sema_t  sem;
        rtos_mutex_t mutex;
    } handle;
    size_t item_size;
    uint32_t waiter_count;
    rtos_task_t lock_owner;
    /* True if created from inside a managed Lua execution (an async job or a
     * synchronous lua_run — both stash __cancel_ptr; a bare REPL does not).
     * Such objects are auto-reclaimed when the job world goes quiescent, so a
     * stopped/crashed orchestrator can't leak them into the next run. Objects
     * created outside any managed run (REPL) stay job_scoped=false and persist
     * until an explicit *_delete. */
    bool job_scoped;
    struct thread_sync_object *next;
} thread_sync_object_t;

typedef struct {
    size_t len;
    uint8_t data[];
} thread_sync_queue_item_t;

static thread_sync_object_t *s_objects;
static rtos_mutex_t s_registry_lock;
static size_t s_object_count;

static char *thread_sync_strdup(const char *value)
{
    size_t len = strlen(value) + 1;
    char *copy = malloc(len);

    if (copy) {
        memcpy(copy, value, len);
    }
    return copy;
}

/* Create the registry mutex if it does not exist yet. The check-then-create is
 * NOT atomic, so it must not be relied on to arbitrate two concurrent callers:
 * cap_lua_init() calls thread_sync_init() ONCE in the single-threaded boot phase
 * (before any job task can run luaopen_thread), which pre-creates the mutex. By
 * the time per-lua_State inits run concurrently this always short-circuits on
 * the first branch, so the non-atomic path below is only ever taken once, at
 * boot. */
static int thread_sync_ensure_lock(void)
{
    if (s_registry_lock) {
        return RTK_SUCCESS;
    }
    return rtos_mutex_create(&s_registry_lock) == RTK_SUCCESS ? RTK_SUCCESS : RTK_FAIL;
}

static void thread_sync_lock_registry(void)
{
    (void)thread_sync_ensure_lock();
    rtos_mutex_take(s_registry_lock, RTOS_MAX_DELAY);
}

static void thread_sync_unlock_registry(void)
{
    rtos_mutex_give(s_registry_lock);
}

static thread_sync_object_t *thread_sync_find_locked(const char *name)
{
    thread_sync_object_t *obj = s_objects;

    while (obj) {
        if (strcmp(obj->name, name) == 0) {
            return obj;
        }
        obj = obj->next;
    }
    return NULL;
}

static const char *thread_sync_check_name(lua_State *L, int index)
{
    size_t len = 0;
    const char *name = luaL_checklstring(L, index, &len);

    if (len == 0 || len > CLAW_LUA_THREAD_SYNC_NAME_MAX) {
        luaL_error(L, "thread.sync: name length must be 1..%d", CLAW_LUA_THREAD_SYNC_NAME_MAX);
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!isalnum(ch) && ch != '_' && ch != '.' && ch != ':' && ch != '-') {
            luaL_error(L, "thread.sync: name contains invalid character");
        }
    }
    return name;
}

static uint32_t thread_sync_opt_uint(lua_State *L,
                                      int opts_idx,
                                      const char *field,
                                      uint32_t default_value,
                                      uint32_t min_value,
                                      uint32_t max_value)
{
    lua_Integer value = default_value;

    if (lua_istable(L, opts_idx)) {
        lua_getfield(L, opts_idx, field);
        if (!lua_isnil(L, -1)) {
            value = luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);
    }

    if (value < (lua_Integer)min_value || value > (lua_Integer)max_value) {
        /* Use %d, NOT %u: luaL_error routes through lua_pushfstring, whose
         * mini-formatter supports only %d/%f/%s/%p/%c/%U/%I/%% — a %u makes it
         * raise the useless "invalid option '%u' to 'lua_pushfstring'" instead
         * of this range hint, which hides the real (trivially fixable) cause
         * of an out-of-range option from the caller. Values are small limits. */
        luaL_error(L, "thread.sync: option '%s' must be %d..%d",
                   field, (int)min_value, (int)max_value);
    }
    return (uint32_t)value;
}

static int thread_sync_push_nil_error(lua_State *L, const char *error)
{
    lua_pushnil(L);
    lua_pushstring(L, error);
    return 2;
}

static int thread_sync_push_false_error(lua_State *L, const char *error)
{
    lua_pushboolean(L, false);
    lua_pushstring(L, error);
    return 2;
}

/* Cooperative cancel check — same convention as lua_module_event.c's
 * lua_event_wait (LUA_REGISTRYINDEX["__cancel_ptr"]). */
static bool thread_sync_cancelled(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__cancel_ptr");
    volatile int *cp = (volatile int *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return cp && *cp;
}

/* True if we are running inside a managed Lua execution (async job or
 * synchronous lua_run), i.e. something that participates in the cap_lua
 * quiescence accounting. Both stash a non-NULL __cancel_ptr lightuserdata; a
 * bare AT+CLAW=lua_repl session does not, so its objects are left persistent.
 * Used only to tag newly created objects for the quiescence sweep. */
static bool thread_sync_managed_exec(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__cancel_ptr");
    bool managed = lua_islightuserdata(L, -1) && lua_touserdata(L, -1) != NULL;
    lua_pop(L, 1);
    return managed;
}

static thread_sync_object_t *thread_sync_acquire(lua_State *L,
                                                   const char *name,
                                                   thread_sync_type_t type)
{
    thread_sync_object_t *obj = NULL;

    thread_sync_lock_registry();
    obj = thread_sync_find_locked(name);
    if (!obj) {
        thread_sync_unlock_registry();
        return NULL;
    }
    if (obj->type != type) {
        thread_sync_unlock_registry();
        luaL_error(L, "thread.sync: object '%s' has a different type", name);
    }
    obj->waiter_count++;
    thread_sync_unlock_registry();
    return obj;
}

static void thread_sync_release(thread_sync_object_t *obj)
{
    thread_sync_lock_registry();
    if (obj->waiter_count > 0) {
        obj->waiter_count--;
    }
    thread_sync_unlock_registry();
}

static size_t thread_sync_queue_storage_size(size_t item_size)
{
    return sizeof(thread_sync_queue_item_t) + item_size;
}

static uint32_t thread_sync_timeout_arg(lua_State *L, int index)
{
    lua_Integer timeout = luaL_optinteger(L, index, 0);

    if (timeout < 0) {
        luaL_error(L, "thread.sync: timeout must be >= 0");
    }
    if ((uint64_t)timeout > UINT32_MAX) {
        luaL_error(L, "thread.sync: timeout is too large");
    }
    return (uint32_t)timeout;
}

static int thread_sync_queue_create(lua_State *L)
{
    const char *name = thread_sync_check_name(L, 1);
    int opts_idx = lua_gettop(L) >= 2 && !lua_isnil(L, 2) ? 2 : 0;
    uint32_t depth = 0;
    uint32_t item_size = 0;
    thread_sync_object_t *obj = NULL;
    size_t storage_size = 0;

    if (opts_idx && !lua_istable(L, opts_idx)) {
        return luaL_error(L, "thread.sync.queue_create: opts must be a table");
    }
    depth = thread_sync_opt_uint(L, opts_idx, "depth",
                                  CLAW_LUA_THREAD_QUEUE_DEPTH_DEFAULT,
                                  1, CLAW_LUA_THREAD_QUEUE_DEPTH_MAX);
    item_size = thread_sync_opt_uint(L, opts_idx, "item_size",
                                      CLAW_LUA_THREAD_QUEUE_ITEM_SIZE_DEFAULT,
                                      1, CLAW_LUA_THREAD_QUEUE_ITEM_SIZE_MAX);
    storage_size = thread_sync_queue_storage_size(item_size);

    obj = calloc(1, sizeof(*obj));
    if (!obj) {
        return thread_sync_push_nil_error(L, "no_mem");
    }
    obj->name = thread_sync_strdup(name);
    obj->type = THREAD_SYNC_TYPE_QUEUE;
    obj->item_size = item_size;
    obj->job_scoped = thread_sync_managed_exec(L);
    if (!obj->name ||
        rtos_queue_create(&obj->handle.queue, depth, storage_size) != RTK_SUCCESS) {
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "no_mem");
    }

    thread_sync_lock_registry();
    if (thread_sync_find_locked(name)) {
        thread_sync_unlock_registry();
        rtos_queue_delete(obj->handle.queue);
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "exists");
    }
    if (s_object_count >= CLAW_LUA_THREAD_SYNC_MAX_OBJECTS) {
        thread_sync_unlock_registry();
        rtos_queue_delete(obj->handle.queue);
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "limit");
    }
    obj->next = s_objects;
    s_objects = obj;
    s_object_count++;
    thread_sync_unlock_registry();

    lua_pushboolean(L, true);
    return 1;
}

static int thread_sync_delete(lua_State *L, thread_sync_type_t type)
{
    const char *name = thread_sync_check_name(L, 1);
    thread_sync_object_t *obj = NULL;
    thread_sync_object_t **link = NULL;

    thread_sync_lock_registry();
    link = &s_objects;
    while (*link && strcmp((*link)->name, name) != 0) {
        link = &(*link)->next;
    }
    obj = *link;
    if (!obj) {
        thread_sync_unlock_registry();
        return thread_sync_push_nil_error(L, "not_found");
    }
    if (obj->type != type) {
        thread_sync_unlock_registry();
        return luaL_error(L, "thread.sync: object '%s' has a different type", name);
    }
    if (obj->waiter_count > 0) {
        thread_sync_unlock_registry();
        return thread_sync_push_nil_error(L, "busy");
    }
    if (obj->type == THREAD_SYNC_TYPE_QUEUE && rtos_queue_message_waiting(obj->handle.queue) > 0) {
        thread_sync_unlock_registry();
        return thread_sync_push_nil_error(L, "busy");
    }
    if (obj->type == THREAD_SYNC_TYPE_LOCK && obj->lock_owner) {
        thread_sync_unlock_registry();
        return thread_sync_push_nil_error(L, "busy");
    }

    *link = obj->next;
    s_object_count--;
    thread_sync_unlock_registry();

    if (obj->type == THREAD_SYNC_TYPE_QUEUE) {
        rtos_queue_delete(obj->handle.queue);
    } else if (obj->type == THREAD_SYNC_TYPE_SEM) {
        rtos_sema_delete(obj->handle.sem);
    } else {
        rtos_mutex_delete(obj->handle.mutex);
    }
    free(obj->name);
    free(obj);

    lua_pushboolean(L, true);
    return 1;
}

static int thread_sync_queue_delete(lua_State *L)
{
    return thread_sync_delete(L, THREAD_SYNC_TYPE_QUEUE);
}

static int thread_sync_queue_send(lua_State *L)
{
    const char *name = thread_sync_check_name(L, 1);
    size_t len = 0;
    const char *data = NULL;
    uint32_t timeout_ms = thread_sync_timeout_arg(L, 3);
    thread_sync_object_t *obj = NULL;
    thread_sync_queue_item_t *item = NULL;
    size_t storage_size = 0;
    int ok = RTK_FAIL;
    uint32_t waited_ms = 0;

    if (lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "thread.sync.queue_send: value must be a string");
    }
    data = lua_tolstring(L, 2, &len);

    obj = thread_sync_acquire(L, name, THREAD_SYNC_TYPE_QUEUE);
    if (!obj) {
        return thread_sync_push_false_error(L, "not_found");
    }
    if (len > obj->item_size) {
        thread_sync_release(obj);
        return luaL_error(L, "thread.sync.queue_send: value exceeds item_size");
    }

    storage_size = thread_sync_queue_storage_size(obj->item_size);
    item = malloc(storage_size);
    if (!item) {
        thread_sync_release(obj);
        return thread_sync_push_false_error(L, "no_mem");
    }
    item->len = len;
    memcpy(item->data, data, len);

    do {
        uint32_t step_ms = 0;
        if (thread_sync_cancelled(L)) {
            free(item);
            thread_sync_release(obj);
            return thread_sync_push_false_error(L, "stopped");
        }
        if (timeout_ms > waited_ms) {
            uint32_t remaining = timeout_ms - waited_ms;
            step_ms = remaining > CLAW_LUA_THREAD_WAIT_STEP_MS ? CLAW_LUA_THREAD_WAIT_STEP_MS : remaining;
        }
        ok = rtos_queue_send(obj->handle.queue, item, step_ms);
        if (ok == RTK_SUCCESS) {
            free(item);
            thread_sync_release(obj);
            lua_pushboolean(L, true);
            return 1;
        }
        waited_ms += step_ms;
    } while (timeout_ms > waited_ms);

    free(item);
    thread_sync_release(obj);
    return thread_sync_push_false_error(L, "timeout");
}

static int thread_sync_queue_recv(lua_State *L)
{
    const char *name = thread_sync_check_name(L, 1);
    uint32_t timeout_ms = thread_sync_timeout_arg(L, 2);
    thread_sync_object_t *obj = thread_sync_acquire(L, name, THREAD_SYNC_TYPE_QUEUE);
    thread_sync_queue_item_t *item = NULL;
    size_t storage_size = 0;
    int ok = RTK_FAIL;
    uint32_t waited_ms = 0;

    if (!obj) {
        return thread_sync_push_nil_error(L, "not_found");
    }

    storage_size = thread_sync_queue_storage_size(obj->item_size);
    item = malloc(storage_size);
    if (!item) {
        thread_sync_release(obj);
        return thread_sync_push_nil_error(L, "no_mem");
    }

    do {
        uint32_t step_ms = 0;
        if (thread_sync_cancelled(L)) {
            free(item);
            thread_sync_release(obj);
            return thread_sync_push_nil_error(L, "stopped");
        }
        if (timeout_ms > waited_ms) {
            uint32_t remaining = timeout_ms - waited_ms;
            step_ms = remaining > CLAW_LUA_THREAD_WAIT_STEP_MS ? CLAW_LUA_THREAD_WAIT_STEP_MS : remaining;
        }
        ok = rtos_queue_receive(obj->handle.queue, item, step_ms);
        if (ok == RTK_SUCCESS) {
            thread_sync_release(obj);
            lua_pushlstring(L, (const char *)item->data, item->len);
            free(item);
            return 1;
        }
        waited_ms += step_ms;
    } while (timeout_ms > waited_ms);

    free(item);
    thread_sync_release(obj);
    return thread_sync_push_nil_error(L, "timeout");
}

static int thread_sync_sem_create(lua_State *L)
{
    const char *name = thread_sync_check_name(L, 1);
    int opts_idx = lua_gettop(L) >= 2 && !lua_isnil(L, 2) ? 2 : 0;
    uint32_t max = 0;
    uint32_t initial = 0;
    thread_sync_object_t *obj = NULL;

    if (opts_idx && !lua_istable(L, opts_idx)) {
        return luaL_error(L, "thread.sync.sem_create: opts must be a table");
    }
    max = thread_sync_opt_uint(L, opts_idx, "max", 1, 1, CLAW_LUA_THREAD_SEM_MAX_COUNT);
    initial = thread_sync_opt_uint(L, opts_idx, "initial", 0, 0, max);

    obj = calloc(1, sizeof(*obj));
    if (!obj) {
        return thread_sync_push_nil_error(L, "no_mem");
    }
    obj->name = thread_sync_strdup(name);
    obj->type = THREAD_SYNC_TYPE_SEM;
    obj->job_scoped = thread_sync_managed_exec(L);
    if (!obj->name ||
        rtos_sema_create(&obj->handle.sem, initial, max) != RTK_SUCCESS) {
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "no_mem");
    }

    thread_sync_lock_registry();
    if (thread_sync_find_locked(name)) {
        thread_sync_unlock_registry();
        rtos_sema_delete(obj->handle.sem);
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "exists");
    }
    if (s_object_count >= CLAW_LUA_THREAD_SYNC_MAX_OBJECTS) {
        thread_sync_unlock_registry();
        rtos_sema_delete(obj->handle.sem);
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "limit");
    }
    obj->next = s_objects;
    s_objects = obj;
    s_object_count++;
    thread_sync_unlock_registry();

    lua_pushboolean(L, true);
    return 1;
}

static int thread_sync_sem_delete(lua_State *L)
{
    return thread_sync_delete(L, THREAD_SYNC_TYPE_SEM);
}

static int thread_sync_sem_give(lua_State *L)
{
    const char *name = thread_sync_check_name(L, 1);
    thread_sync_object_t *obj = thread_sync_acquire(L, name, THREAD_SYNC_TYPE_SEM);
    int ok = RTK_FAIL;

    if (!obj) {
        return thread_sync_push_false_error(L, "not_found");
    }
    ok = rtos_sema_give(obj->handle.sem);
    thread_sync_release(obj);
    if (ok != RTK_SUCCESS) {
        return thread_sync_push_false_error(L, "full");
    }
    lua_pushboolean(L, true);
    return 1;
}

static int thread_sync_take_common(lua_State *L, thread_sync_type_t type)
{
    const char *name = thread_sync_check_name(L, 1);
    uint32_t timeout_ms = thread_sync_timeout_arg(L, 2);
    thread_sync_object_t *obj = thread_sync_acquire(L, name, type);
    void *handle = NULL;
    int ok = RTK_FAIL;
    uint32_t waited_ms = 0;

    if (!obj) {
        return thread_sync_push_false_error(L, "not_found");
    }
    handle = type == THREAD_SYNC_TYPE_SEM ? obj->handle.sem : obj->handle.mutex;

    do {
        uint32_t step_ms = 0;
        if (thread_sync_cancelled(L)) {
            thread_sync_release(obj);
            return thread_sync_push_false_error(L, "stopped");
        }
        if (timeout_ms > waited_ms) {
            uint32_t remaining = timeout_ms - waited_ms;
            step_ms = remaining > CLAW_LUA_THREAD_WAIT_STEP_MS ? CLAW_LUA_THREAD_WAIT_STEP_MS : remaining;
        }
        ok = type == THREAD_SYNC_TYPE_SEM
                 ? rtos_sema_take((rtos_sema_t)handle, step_ms)
                 : rtos_mutex_take((rtos_mutex_t)handle, step_ms);
        if (ok == RTK_SUCCESS) {
            if (type == THREAD_SYNC_TYPE_LOCK) {
                thread_sync_lock_registry();
                obj->lock_owner = rtos_task_handle_get();
                thread_sync_unlock_registry();
            }
            thread_sync_release(obj);
            lua_pushboolean(L, true);
            return 1;
        }
        waited_ms += step_ms;
    } while (timeout_ms > waited_ms);

    thread_sync_release(obj);
    return thread_sync_push_false_error(L, "timeout");
}

static int thread_sync_sem_take(lua_State *L)
{
    return thread_sync_take_common(L, THREAD_SYNC_TYPE_SEM);
}

static int thread_sync_lock_create(lua_State *L)
{
    const char *name = thread_sync_check_name(L, 1);
    thread_sync_object_t *obj = calloc(1, sizeof(*obj));

    if (!obj) {
        return thread_sync_push_nil_error(L, "no_mem");
    }
    obj->name = thread_sync_strdup(name);
    obj->type = THREAD_SYNC_TYPE_LOCK;
    obj->job_scoped = thread_sync_managed_exec(L);
    if (!obj->name || rtos_mutex_create(&obj->handle.mutex) != RTK_SUCCESS) {
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "no_mem");
    }

    thread_sync_lock_registry();
    if (thread_sync_find_locked(name)) {
        thread_sync_unlock_registry();
        rtos_mutex_delete(obj->handle.mutex);
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "exists");
    }
    if (s_object_count >= CLAW_LUA_THREAD_SYNC_MAX_OBJECTS) {
        thread_sync_unlock_registry();
        rtos_mutex_delete(obj->handle.mutex);
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "limit");
    }
    obj->next = s_objects;
    s_objects = obj;
    s_object_count++;
    thread_sync_unlock_registry();

    lua_pushboolean(L, true);
    return 1;
}

static int thread_sync_lock_delete(lua_State *L)
{
    return thread_sync_delete(L, THREAD_SYNC_TYPE_LOCK);
}

static int thread_sync_lock_take(lua_State *L)
{
    return thread_sync_take_common(L, THREAD_SYNC_TYPE_LOCK);
}

static int thread_sync_unlock(lua_State *L)
{
    const char *name = thread_sync_check_name(L, 1);
    thread_sync_object_t *obj = thread_sync_acquire(L, name, THREAD_SYNC_TYPE_LOCK);
    rtos_task_t current = rtos_task_handle_get();
    int ok = RTK_FAIL;

    if (!obj) {
        return thread_sync_push_false_error(L, "not_found");
    }

    thread_sync_lock_registry();
    if (obj->lock_owner != current) {
        thread_sync_unlock_registry();
        thread_sync_release(obj);
        return thread_sync_push_false_error(L, "not_owner");
    }
    obj->lock_owner = NULL;
    thread_sync_unlock_registry();

    ok = rtos_mutex_give(obj->handle.mutex);
    thread_sync_release(obj);
    if (ok != RTK_SUCCESS) {
        return thread_sync_push_false_error(L, "unlock_failed");
    }
    lua_pushboolean(L, true);
    return 1;
}

/* Delete an object's underlying RTOS handle by type. Caller has already
 * unlinked it from the list and owns it. */
static void thread_sync_destroy_handle(thread_sync_object_t *obj)
{
    if (obj->type == THREAD_SYNC_TYPE_QUEUE) {
        rtos_queue_delete(obj->handle.queue);
    } else if (obj->type == THREAD_SYNC_TYPE_SEM) {
        rtos_sema_delete(obj->handle.sem);
    } else {
        rtos_mutex_delete(obj->handle.mutex);
    }
}

/* Quiescence callback (registered with cap_lua): reclaim every job-scoped
 * object. Invoked only when no async job and no synchronous lua_run is active,
 * so no Lua code can be blocked inside acquire/release — the objects are truly
 * idle and safe to delete outright. The waiter_count==0 test is a belt-and-
 * suspenders guard: if some object were somehow still in use it is skipped and
 * swept at the next quiescence rather than pulled out from under a waiter.
 * REPL-created objects (job_scoped=false) are left untouched. */
static void thread_sync_reclaim_job_scoped(void)
{
    thread_sync_lock_registry();
    thread_sync_object_t **link = &s_objects;
    while (*link) {
        thread_sync_object_t *obj = *link;
        if (obj->job_scoped && obj->waiter_count == 0) {
            *link = obj->next;
            s_object_count--;
            thread_sync_destroy_handle(obj);
            free(obj->name);
            free(obj);
        } else {
            link = &obj->next;
        }
    }
    thread_sync_unlock_registry();
}

int lua_module_thread_push_sync(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"queue_create", thread_sync_queue_create},
        {"queue_send", thread_sync_queue_send},
        {"queue_recv", thread_sync_queue_recv},
        {"queue_delete", thread_sync_queue_delete},
        {"sem_create", thread_sync_sem_create},
        {"sem_give", thread_sync_sem_give},
        {"sem_take", thread_sync_sem_take},
        {"sem_delete", thread_sync_sem_delete},
        {"lock_create", thread_sync_lock_create},
        {"lock", thread_sync_lock_take},
        {"unlock", thread_sync_unlock},
        {"lock_delete", thread_sync_lock_delete},
        {NULL, NULL},
    };

    if (thread_sync_ensure_lock() != RTK_SUCCESS) {
        luaL_error(L, "thread.sync: failed to create registry lock");
    }

    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

int thread_sync_init(void)
{
    /* Register the quiescence sweep so job-scoped sync objects can't leak past
     * the run that created them. Idempotent: called on every lua_State open,
     * always sets the same function pointer. */
    cap_lua_set_quiescence_cb(thread_sync_reclaim_job_scoped);
    return thread_sync_ensure_lock();
}
