/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * claw_cap.c — Capability registry for the Ameba claw AI agent.
 *
 * Design:
 *   - Fixed-capacity static tables (no realloc).
 *   - Two open-addressed hash tables (FNV-1a, linear probe) for O(1)
 *     lookup by name and by id.
 *   - Two independent mutexes: s_reg_lock (registry) and s_vis_lock
 *     (visibility lists).
 *   - execute() callbacks are always called outside any lock; inflight
 *     counters gate concurrent unregister.
 */

#include "claw_cap.h"
#include "os_wrapper.h"
#include "memproc.h"

#include <stdbool.h>

#include "cJSON.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define CAP_MAX_GROUPS    24
#define CAP_MAX_DESCS     96
#define CAP_HT_BITS       8
#define CAP_HT_SIZE       (1 << CAP_HT_BITS)   /* 256 */
#define CAP_HT_MASK       (CAP_HT_SIZE - 1)

#define CAP_MAX_SESSIONS  8
#define CAP_SESSION_GIDS  8
#define CAP_VIS_MAX       24

#define CAP_UNLOAD_TICK_MS  20
#define CAP_DESC_TRUNC_LEN  256

static const char *TAG = "cap";

/* ------------------------------------------------------------------ */
/*  Internal types                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const claw_cap_group_t *group;      /* caller keeps alive */
    claw_cap_state_t        state;
    uint16_t                desc_start; /* index into s_descs */
    uint16_t                desc_count;
    bool                    init_called;
    bool                    live;
} group_entry_t;

typedef struct {
    const claw_cap_descriptor_t *desc;  /* points to group->descriptors[i] */
    uint8_t                      grp_idx;
    claw_cap_state_t             state;
    bool                         init_called;
    bool                         live;
    uint16_t                     active_calls;
} desc_entry_t;

typedef struct {
    uint32_t hash;
    uint16_t slot_idx;  /* index into s_descs */
    uint8_t  used;
} ht_bucket_t;

typedef struct {
    char sid[64];
    char gids[CAP_SESSION_GIDS][64];
    int  gid_cnt;
} vis_scope_t;

/* ------------------------------------------------------------------ */
/*  Static storage                                                     */
/* ------------------------------------------------------------------ */

static group_entry_t  s_groups[CAP_MAX_GROUPS];
static desc_entry_t   s_descs[CAP_MAX_DESCS];

static ht_bucket_t    s_name_ht[CAP_HT_SIZE];
static ht_bucket_t    s_id_ht[CAP_HT_SIZE];

/* snapshot arrays for claw_cap_list / claw_cap_list_groups */
static claw_cap_descriptor_t   s_desc_snap[CAP_MAX_DESCS];
static claw_cap_group_info_t   s_grp_snap[CAP_MAX_GROUPS];

/* global visibility list */
static char s_vis_gids[CAP_VIS_MAX][64];
static int  s_vis_cnt;

/* per-session visibility scopes */
static vis_scope_t s_scopes[CAP_MAX_SESSIONS];

static rtos_mutex_t s_reg_lock;
static rtos_mutex_t s_vis_lock;
static bool              s_initialized;
static bool              s_started;   /* true after claw_cap_start_all */

/* ------------------------------------------------------------------ */
/*  Lock helpers                                                       */
/* ------------------------------------------------------------------ */

static void reg_take(void)  { rtos_mutex_take(s_reg_lock, 0xFFFFFFFFUL); }
static void reg_give(void)  { rtos_mutex_give(s_reg_lock); }
static void vis_take(void)  { rtos_mutex_take(s_vis_lock, 0xFFFFFFFFUL); }
static void vis_give(void)  { rtos_mutex_give(s_vis_lock); }

/* ------------------------------------------------------------------ */
/*  FNV-1a hash                                                        */
/* ------------------------------------------------------------------ */

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;

    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/*  Hash table operations — open addressing, linear probe             */
/* ------------------------------------------------------------------ */

/* Insert by name. Returns false if table is full. */
static bool ht_insert_name(uint16_t di)
{
    const char *name = s_descs[di].desc->name;
    uint32_t    h    = fnv1a(name);
    uint16_t    pos  = (uint16_t)(h & CAP_HT_MASK);
    int         i;

    for (i = 0; i < CAP_HT_SIZE; i++) {
        if (!s_name_ht[pos].used) {
            s_name_ht[pos].hash     = h;
            s_name_ht[pos].slot_idx = di;
            s_name_ht[pos].used     = 1;
            return true;
        }
        pos = (uint16_t)((pos + 1) & CAP_HT_MASK);
    }
    return false;
}

/* Lookup by name; returns desc index or -1. */
static int ht_lookup_name(const char *name)
{
    uint32_t h   = fnv1a(name);
    uint16_t pos = (uint16_t)(h & CAP_HT_MASK);
    int      i;

    for (i = 0; i < CAP_HT_SIZE; i++) {
        if (!s_name_ht[pos].used) {
            return -1;
        }
        if (s_name_ht[pos].hash == h) {
            uint16_t di = s_name_ht[pos].slot_idx;
            if (s_descs[di].live &&
                s_descs[di].desc &&
                strcmp(s_descs[di].desc->name, name) == 0) {
                return (int)di;
            }
        }
        pos = (uint16_t)((pos + 1) & CAP_HT_MASK);
    }
    return -1;
}

/* Remove by name — tombstone not needed; we mark bucket unused and
 * rehash the subsequent run to avoid breaking probe chains.         */
static void ht_remove_name(uint16_t di)
{
    const char *name = s_descs[di].desc ? s_descs[di].desc->name : NULL;
    uint32_t    h;
    uint16_t    pos, scan, j;
    int         i;

    if (!name) {
        return;
    }
    h   = fnv1a(name);
    pos = (uint16_t)(h & CAP_HT_MASK);

    for (i = 0; i < CAP_HT_SIZE; i++) {
        if (!s_name_ht[pos].used) {
            return;
        }
        if (s_name_ht[pos].hash == h && s_name_ht[pos].slot_idx == di) {
            break;
        }
        pos = (uint16_t)((pos + 1) & CAP_HT_MASK);
    }
    if (i == CAP_HT_SIZE) {
        return;
    }

    /* Robin-Hood backward shift deletion */
    s_name_ht[pos].used = 0;
    scan = (uint16_t)((pos + 1) & CAP_HT_MASK);
    while (s_name_ht[scan].used) {
        j = (uint16_t)(s_name_ht[scan].hash & CAP_HT_MASK);
        /* check if scan's natural slot is "behind" pos in the table */
        if ((scan > pos && (j <= pos || j > scan)) ||
            (scan < pos && (j <= pos && j > scan))) {
            s_name_ht[pos]  = s_name_ht[scan];
            s_name_ht[scan].used = 0;
            pos = scan;
        }
        scan = (uint16_t)((scan + 1) & CAP_HT_MASK);
    }
}

/* Insert by id. */
static bool ht_insert_id(uint16_t di)
{
    const char *id  = s_descs[di].desc->id;
    uint32_t    h   = fnv1a(id);
    uint16_t    pos = (uint16_t)(h & CAP_HT_MASK);
    int         i;

    for (i = 0; i < CAP_HT_SIZE; i++) {
        if (!s_id_ht[pos].used) {
            s_id_ht[pos].hash     = h;
            s_id_ht[pos].slot_idx = di;
            s_id_ht[pos].used     = 1;
            return true;
        }
        pos = (uint16_t)((pos + 1) & CAP_HT_MASK);
    }
    return false;
}

/* Lookup by id; returns desc index or -1. */
static int ht_lookup_id(const char *id)
{
    uint32_t h   = fnv1a(id);
    uint16_t pos = (uint16_t)(h & CAP_HT_MASK);
    int      i;

    for (i = 0; i < CAP_HT_SIZE; i++) {
        if (!s_id_ht[pos].used) {
            return -1;
        }
        if (s_id_ht[pos].hash == h) {
            uint16_t di = s_id_ht[pos].slot_idx;
            if (s_descs[di].live &&
                s_descs[di].desc &&
                strcmp(s_descs[di].desc->id, id) == 0) {
                return (int)di;
            }
        }
        pos = (uint16_t)((pos + 1) & CAP_HT_MASK);
    }
    return -1;
}

static void ht_remove_id(uint16_t di)
{
    const char *id = s_descs[di].desc ? s_descs[di].desc->id : NULL;
    uint32_t    h;
    uint16_t    pos, scan, j;
    int         i;

    if (!id) {
        return;
    }
    h   = fnv1a(id);
    pos = (uint16_t)(h & CAP_HT_MASK);

    for (i = 0; i < CAP_HT_SIZE; i++) {
        if (!s_id_ht[pos].used) {
            return;
        }
        if (s_id_ht[pos].hash == h && s_id_ht[pos].slot_idx == di) {
            break;
        }
        pos = (uint16_t)((pos + 1) & CAP_HT_MASK);
    }
    if (i == CAP_HT_SIZE) {
        return;
    }

    s_id_ht[pos].used = 0;
    scan = (uint16_t)((pos + 1) & CAP_HT_MASK);
    while (s_id_ht[scan].used) {
        j = (uint16_t)(s_id_ht[scan].hash & CAP_HT_MASK);
        if ((scan > pos && (j <= pos || j > scan)) ||
            (scan < pos && (j <= pos && j > scan))) {
            s_id_ht[pos]  = s_id_ht[scan];
            s_id_ht[scan].used = 0;
            pos = scan;
        }
        scan = (uint16_t)((scan + 1) & CAP_HT_MASK);
    }
}

/* ------------------------------------------------------------------ */
/*  Registry allocation helpers                                        */
/* ------------------------------------------------------------------ */

/* Find a free group slot; returns index or -1. */
static int reg_alloc_group(void)
{
    int i;

    for (i = 0; i < CAP_MAX_GROUPS; i++) {
        if (!s_groups[i].live) {
            return i;
        }
    }
    return -1;
}

/* Find a run of [count] consecutive free desc slots starting at a
 * contiguous block.  Returns start index or -1.                     */
static int reg_alloc_desc(int count)
{
    int i, j;

    for (i = 0; i <= CAP_MAX_DESCS - count; i++) {
        bool ok = true;

        for (j = 0; j < count; j++) {
            if (s_descs[i + j].live) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return i;
        }
    }
    return -1;
}

/* Release a group slot and its desc slots (caller must have called
 * ht_remove_* and life_stop_group first).                           */
static void reg_free_group(int gi)
{
    group_entry_t *ge = &s_groups[gi];
    int i;

    for (i = 0; i < ge->desc_count; i++) {
        int di = ge->desc_start + i;

        ht_remove_name((uint16_t)di);
        ht_remove_id((uint16_t)di);
        _memset(&s_descs[di], 0, sizeof(s_descs[di]));
    }
    _memset(ge, 0, sizeof(*ge));
}

static void __attribute__((unused)) reg_free_desc(int di)
{
    ht_remove_name((uint16_t)di);
    ht_remove_id((uint16_t)di);
    _memset(&s_descs[di], 0, sizeof(s_descs[di]));
}

/* ------------------------------------------------------------------ */
/*  Visibility helpers                                                 */
/* ------------------------------------------------------------------ */

static bool vis_gid_in_list(const char *gid, const char list[][64], int cnt)
{
    int i;

    for (i = 0; i < cnt; i++) {
        if (strcmp(list[i], gid) == 0) {
            return true;
        }
    }
    return false;
}

/* Returns scope index for session_id, or -1. */
static int vis_scope_find(const char *sid)
{
    int i;

    if (!sid || !sid[0]) {
        return -1;
    }
    for (i = 0; i < CAP_MAX_SESSIONS; i++) {
        if (s_scopes[i].sid[0] && strcmp(s_scopes[i].sid, sid) == 0) {
            return i;
        }
    }
    return -1;
}

static const vis_scope_t *vis_scope_get(const char *sid)
{
    int i = vis_scope_find(sid);

    return i >= 0 ? &s_scopes[i] : NULL;
}

static int vis_scope_set(const char *sid,
                         const char *const *gids, size_t gid_cnt)
{
    int   i;
    int   free_slot = -1;
    size_t k;

    if (!sid || !sid[0]) {
        return RTK_ERR_BADARG;
    }
    if (gid_cnt > CAP_SESSION_GIDS) {
        return RTK_ERR_BADARG;
    }

    vis_take();

    /* Find existing or allocate new slot */
    for (i = 0; i < CAP_MAX_SESSIONS; i++) {
        if (s_scopes[i].sid[0]) {
            if (strcmp(s_scopes[i].sid, sid) == 0) {
                free_slot = i;
                break;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot < 0) {
        vis_give();
        return RTK_ERR_NOMEM;
    }

    if (gid_cnt == 0) {
        /* Clear the scope */
        _memset(&s_scopes[free_slot], 0, sizeof(s_scopes[free_slot]));
        vis_give();
        return RTK_SUCCESS;
    }

    _memset(&s_scopes[free_slot], 0, sizeof(s_scopes[free_slot]));
    strlcpy(s_scopes[free_slot].sid, sid, sizeof(s_scopes[free_slot].sid));
    for (k = 0; k < gid_cnt; k++) {
        if (!gids[k] || !gids[k][0]) {
            _memset(&s_scopes[free_slot], 0, sizeof(s_scopes[free_slot]));
            vis_give();
            return RTK_ERR_BADARG;
        }
        strlcpy(s_scopes[free_slot].gids[k], gids[k],
                sizeof(s_scopes[free_slot].gids[k]));
    }
    s_scopes[free_slot].gid_cnt = (int)gid_cnt;

    vis_give();
    return RTK_SUCCESS;
}

static void vis_set_global(const char *const *gids, size_t cnt)
{
    size_t i;

    vis_take();
    _memset(s_vis_gids, 0, sizeof(s_vis_gids));
    s_vis_cnt = 0;
    for (i = 0; i < cnt && i < CAP_VIS_MAX; i++) {
        if (gids[i] && gids[i][0]) {
            strlcpy(s_vis_gids[i], gids[i], sizeof(s_vis_gids[i]));
            s_vis_cnt++;
        }
    }
    vis_give();
}

/* Check whether a desc_entry is accessible to LLM for a given session.
 * Caller does NOT need to hold s_vis_lock (reads are snapshot-safe for
 * the purposes of visibility decisions).                             */
static bool vis_check_desc(const desc_entry_t *de, const char *sid)
{
    const claw_cap_descriptor_t *d;
    const char                  *gid;
    const vis_scope_t           *sc;

    if (!de || !de->live || !de->desc) {
        return false;
    }
    d = de->desc;

    /* Must be a callable kind */
    if (d->kind != CLAW_CAP_KIND_INVOKE &&
        d->kind != CLAW_CAP_KIND_BOTH) {
        return false;
    }
    if (!(d->cap_flags & CLAW_CAP_FLAG_LLM_ACCESS)) {
        return false;
    }
    /* Restricted caps are never LLM-visible */
    if (d->cap_flags & CLAW_CAP_FLAG_GUARDED) {
        return false;
    }
    if (!d->execute) {
        return false;
    }

    /* Check group-level visibility */
    if (de->grp_idx >= CAP_MAX_GROUPS ||
        !s_groups[de->grp_idx].live ||
        !s_groups[de->grp_idx].group ||
        !s_groups[de->grp_idx].group->group_id) {
        return false;
    }
    gid = s_groups[de->grp_idx].group->group_id;

    /* If no global list set, everything is visible */
    if (s_vis_cnt == 0) {
        sc = vis_scope_get(sid);
        /* If there is a scope for this session, still OK — we only
         * narrow visibility when the global list is non-empty.      */
        (void)sc;
        return true;
    }

    if (vis_gid_in_list(gid, (const char (*)[64])s_vis_gids, s_vis_cnt)) {
        return true;
    }
    sc = vis_scope_get(sid);
    if (sc && vis_gid_in_list(gid, (const char (*)[64])sc->gids, sc->gid_cnt)) {
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Descriptor / group lookup (registry must be held)                 */
/* ------------------------------------------------------------------ */

/* Returns desc index for id_or_name, or -1. */
static int reg_find_desc(const char *id_or_name)
{
    int di;

    if (!id_or_name || !id_or_name[0]) {
        return -1;
    }
    di = ht_lookup_id(id_or_name);
    if (di >= 0) {
        return di;
    }
    return ht_lookup_name(id_or_name);
}

/* Returns group index for group_id, or -1. */
static int reg_find_group(const char *gid)
{
    int i;

    if (!gid || !gid[0]) {
        return -1;
    }
    for (i = 0; i < CAP_MAX_GROUPS; i++) {
        if (s_groups[i].live &&
            s_groups[i].group &&
            s_groups[i].group->group_id &&
            strcmp(s_groups[i].group->group_id, gid) == 0) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Desc ready checks (must hold s_reg_lock)                         */
/* ------------------------------------------------------------------ */

static bool desc_is_ready(const desc_entry_t *de)
{
    return de &&
           de->live &&
           (de->state == CLAW_CAP_STATE_LOADED ||
            de->state == CLAW_CAP_STATE_ACTIVE);
}

/* ------------------------------------------------------------------ */
/*  JSON helpers                                                       */
/* ------------------------------------------------------------------ */

static void json_write_desc(cJSON *obj,
                             const char *description,
                             const char *cap_name)
{
    const char *src = description ? description : "";
    size_t      len = strlen(src);

    if (len <= CAP_DESC_TRUNC_LEN) {
        cJSON_AddStringToObject(obj, "description", src);
        return;
    }

    RTK_LOGD(TAG, "cap '%s' description %u bytes > %u, truncating\n",
             cap_name ? cap_name : "?",
             (unsigned)len, (unsigned)CAP_DESC_TRUNC_LEN);

    char buf[CAP_DESC_TRUNC_LEN + 1];

    _memcpy(buf, src, CAP_DESC_TRUNC_LEN);
    buf[CAP_DESC_TRUNC_LEN] = '\0';

    /* Back off over any trailing UTF-8 continuation bytes */
    size_t out = CAP_DESC_TRUNC_LEN;

    while (out > 0 && ((unsigned char)buf[out - 1] & 0xC0) == 0x80) {
        buf[--out] = '\0';
    }
    /* Back off over the leading byte of a multi-byte sequence */
    if (out > 0 && ((unsigned char)buf[out - 1] & 0xC0) == 0xC0) {
        buf[--out] = '\0';
    }
    cJSON_AddStringToObject(obj, "description", buf);
}

/* Build the OpenAI function-calling array for one desc entry.
 * Returns a cJSON* object to be added to an array, or NULL on OOM. */
static cJSON *json_build_one_tool(const desc_entry_t *de)
{
    const claw_cap_descriptor_t *d = de->desc;
    cJSON *tool   = cJSON_CreateObject();
    cJSON *func   = cJSON_CreateObject();
    cJSON *schema = NULL;

    if (!tool || !func) {
        cJSON_Delete(tool);
        cJSON_Delete(func);
        return NULL;
    }

    cJSON_AddStringToObject(tool, "type", "function");

    cJSON_AddStringToObject(func, "name", d->name);
    json_write_desc(func, d->description, d->name);

    schema = cJSON_Parse(d->input_schema_json ?
                         d->input_schema_json :
                         "{\"type\":\"object\",\"properties\":{}}");
    if (!schema) {
        schema = cJSON_CreateObject();
        if (!schema) {
            cJSON_Delete(tool);
            cJSON_Delete(func);
            return NULL;
        }
    } else {
        cJSON *t = cJSON_GetObjectItem(schema, "type");
        if (cJSON_IsString(t) &&
            strcmp(t->valuestring, "object") == 0 &&
            !cJSON_GetObjectItem(schema, "properties")) {
            RTK_LOGW(TAG, "cap '%s' schema missing properties\n", d->name);
        }
    }

    cJSON_AddItemToObject(func, "parameters", schema);
    cJSON_AddItemToObject(tool, "function", func);
    return tool;
}

static char *json_build_llm_tools(const claw_cap_call_context_t *ctx,
                                   bool wrap_for_responses_api)
{
    const char *sid = (ctx && ctx->session_id && ctx->session_id[0])
                      ? ctx->session_id : NULL;
    cJSON      *arr = cJSON_CreateArray();
    char       *out = NULL;
    int         i;

    if (!arr) {
        return NULL;
    }

    reg_take();
    for (i = 0; i < CAP_MAX_DESCS; i++) {
        desc_entry_t *de = &s_descs[i];
        cJSON        *tool;

        if (!desc_is_ready(de)) {
            continue;
        }
        if (!vis_check_desc(de, sid)) {
            continue;
        }

        tool = json_build_one_tool(de);
        if (!tool) {
            reg_give();
            cJSON_Delete(arr);
            return NULL;
        }
        cJSON_AddItemToArray(arr, tool);
    }
    reg_give();

    if (!wrap_for_responses_api) {
        out = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        return out;
    }

    /* Wrap as {"tools": [...]} */
    {
        cJSON *wrapper = cJSON_CreateObject();

        if (!wrapper) {
            cJSON_Delete(arr);
            return NULL;
        }
        cJSON_AddItemToObject(wrapper, "tools", arr);
        out = cJSON_PrintUnformatted(wrapper);
        cJSON_Delete(wrapper);
        return out;
    }
}

static char *json_build_catalog(void)
{
    /* Use a local fixed-size buffer, then duplicate to heap.
     * 4096 bytes covers most reasonable catalogs; we grow once if needed. */
    char  buf_stack[4096];
    char *buf   = buf_stack;
    size_t cap  = sizeof(buf_stack);
    size_t off  = 0;
    char  *heap = NULL;
    int    i, n;

    n = DiagSnPrintf(buf + off, cap - off, "Available capabilities:\n");
    if (n > 0) {
        off += (size_t)n;
    }

    reg_take();
    for (i = 0; i < CAP_MAX_DESCS; i++) {
        desc_entry_t                *de = &s_descs[i];
        const claw_cap_descriptor_t *d;

        if (!desc_is_ready(de)) {
            continue;
        }
        d = de->desc;
        n = DiagSnPrintf(buf + off, cap - off,
                     "  [%s] %s \xe2\x80\x94 %s\n",
                     d->id ? d->id : "",
                     d->name ? d->name : "",
                     d->description ? d->description : "");
        if (n > 0 && (size_t)n < cap - off) {
            off += (size_t)n;
        }
        /* Silently skip entries that overflow the buffer */
    }
    reg_give();

    /* Duplicate to heap so caller can free() */
    heap = rtos_mem_malloc(off + 1);
    if (!heap) {
        return NULL;
    }
    _memcpy(heap, buf, off);
    heap[off] = '\0';
    return heap;
}

/* ------------------------------------------------------------------ */
/*  Lifecycle helpers (called WITHOUT s_reg_lock held)                */
/* ------------------------------------------------------------------ */

static int life_start_group(int gi)
{
    group_entry_t               *ge = &s_groups[gi];
    const claw_cap_group_t      *g  = ge->group;
    int                          rc = RTK_SUCCESS;
    int                          i;

    /* group-level init (once) */
    if (!ge->init_called && g && g->group_init) {
        rc = g->group_init();
        if (rc != RTK_SUCCESS) {
            return rc;
        }
        ge->init_called = true;
    }

    /* group-level start */
    if (g && g->group_start) {
        rc = g->group_start();
        if (rc != RTK_SUCCESS) {
            return rc;
        }
    }

    /* per-descriptor init (once) + start */
    for (i = 0; i < ge->desc_count; i++) {
        int            di = ge->desc_start + i;
        desc_entry_t  *de = &s_descs[di];

        if (!de->live) {
            continue;
        }
        if (!de->init_called && de->desc->init) {
            rc = de->desc->init();
            if (rc != RTK_SUCCESS) {
                return rc;
            }
            de->init_called = true;
        }
        if (de->desc->start) {
            rc = de->desc->start();
            if (rc != RTK_SUCCESS) {
                return rc;
            }
        }
    }
    return RTK_SUCCESS;
}

/* Stop in reverse descriptor order, then group stop.
 * Always returns first non-zero error, but continues through all. */
static int life_stop_group(int gi)
{
    group_entry_t          *ge       = &s_groups[gi];
    const claw_cap_group_t *g        = ge->group;
    int                     first_rc = RTK_SUCCESS;
    int                     rc;
    int                     i;

    for (i = ge->desc_count - 1; i >= 0; i--) {
        int           di = ge->desc_start + i;
        desc_entry_t *de = &s_descs[di];

        if (!de->live || !de->desc->stop) {
            continue;
        }
        rc = de->desc->stop();
        if (rc != RTK_SUCCESS && first_rc == RTK_SUCCESS) {
            first_rc = rc;
        }
    }

    if (g && g->group_stop) {
        rc = g->group_stop();
        if (rc != RTK_SUCCESS && first_rc == RTK_SUCCESS) {
            first_rc = rc;
        }
    }
    return first_rc;
}

/* ------------------------------------------------------------------ */
/*  Group state propagation (must hold s_reg_lock)                    */
/* ------------------------------------------------------------------ */

static void grp_set_state(int gi, claw_cap_state_t st)
{
    group_entry_t *ge = &s_groups[gi];
    int            i;

    ge->state = st;
    for (i = 0; i < ge->desc_count; i++) {
        int di = ge->desc_start + i;

        if (s_descs[di].live) {
            s_descs[di].state = st;
        }
    }
}

static bool grp_any_inflight(int gi)
{
    group_entry_t *ge = &s_groups[gi];
    int            i;

    for (i = 0; i < ge->desc_count; i++) {
        int di = ge->desc_start + i;

        if (s_descs[di].live && s_descs[di].active_calls > 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Validation                                                         */
/* ------------------------------------------------------------------ */

static int desc_validate(const claw_cap_descriptor_t *d)
{
    if (!d || !d->id || !d->id[0] || !d->name || !d->name[0]) {
        return RTK_ERR_BADARG;
    }
    if ((d->kind == CLAW_CAP_KIND_INVOKE ||
         d->kind == CLAW_CAP_KIND_BOTH) &&
        (d->cap_flags & CLAW_CAP_FLAG_LLM_ACCESS) &&
        !d->execute) {
        return RTK_ERR_BADARG;
    }
    return RTK_SUCCESS;
}

static int group_validate(const claw_cap_group_t *g)
{
    size_t i, j;

    if (!g || !g->group_id || !g->group_id[0] ||
        !g->descriptors || g->descriptor_count == 0) {
        return RTK_ERR_BADARG;
    }
    if (g->descriptor_count > CAP_MAX_DESCS) {
        return RTK_ERR_BADARG;
    }
    if (reg_find_group(g->group_id) >= 0) {
        return RTK_FAIL; /* already registered */
    }
    for (i = 0; i < g->descriptor_count; i++) {
        int rc = desc_validate(&g->descriptors[i]);

        if (rc != RTK_SUCCESS) {
            return rc;
        }
        /* Check for collisions with already-registered descs */
        if (ht_lookup_id(g->descriptors[i].id) >= 0 ||
            ht_lookup_name(g->descriptors[i].name) >= 0) {
            return RTK_FAIL;
        }
        /* Check for duplicates within the group */
        for (j = i + 1; j < g->descriptor_count; j++) {
            if (strcmp(g->descriptors[i].id,   g->descriptors[j].id)   == 0 ||
                strcmp(g->descriptors[i].name, g->descriptors[j].name) == 0) {
                return RTK_FAIL;
            }
        }
    }
    return RTK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int claw_cap_init(void)
{
    if (s_initialized) {
        return RTK_FAIL;
    }

    _memset(s_groups,   0, sizeof(s_groups));
    _memset(s_descs,    0, sizeof(s_descs));
    _memset(s_name_ht,  0, sizeof(s_name_ht));
    _memset(s_id_ht,    0, sizeof(s_id_ht));
    _memset(s_vis_gids, 0, sizeof(s_vis_gids));
    _memset(s_scopes,   0, sizeof(s_scopes));
    s_vis_cnt   = 0;
    s_started   = false;

    if (rtos_mutex_create(&s_reg_lock) != RTK_SUCCESS) {
        return RTK_ERR_NOMEM;
    }
    if (rtos_mutex_create(&s_vis_lock) != RTK_SUCCESS) {
        rtos_mutex_delete(&s_reg_lock);
        return RTK_ERR_NOMEM;
    }

    s_initialized = true;
    RTK_LOGI(TAG, "cap registry initialized\n");
    return RTK_SUCCESS;
}

int claw_cap_register(const claw_cap_descriptor_t *descriptor)
{
    claw_cap_group_t g;

    if (!descriptor) {
        return RTK_ERR_BADARG;
    }
    _memset(&g, 0, sizeof(g));
    g.group_id        = descriptor->id;
    g.plugin_name     = descriptor->name;
    g.version         = "1";
    g.descriptors     = descriptor;
    g.descriptor_count = 1;
    return claw_cap_register_group(&g);
}

int claw_cap_register_group(const claw_cap_group_t *group)
{
    int gi, base;
    int rc;
    int i;

    if (!s_initialized) {
        return RTK_FAIL;
    }
    if (!group) {
        return RTK_ERR_BADARG;
    }

    reg_take();

    rc = group_validate(group);
    if (rc != RTK_SUCCESS) {
        reg_give();
        return rc;
    }

    gi = reg_alloc_group();
    if (gi < 0) {
        reg_give();
        return RTK_ERR_NOMEM;
    }

    base = reg_alloc_desc((int)group->descriptor_count);
    if (base < 0) {
        reg_give();
        return RTK_ERR_NOMEM;
    }

    /* Fill group entry */
    s_groups[gi].group      = group;
    s_groups[gi].state      = CLAW_CAP_STATE_LOADED;
    s_groups[gi].desc_start = (uint16_t)base;
    s_groups[gi].desc_count = (uint16_t)group->descriptor_count;
    s_groups[gi].init_called = false;
    s_groups[gi].live       = true;

    /* Fill desc entries and hash tables */
    for (i = 0; i < (int)group->descriptor_count; i++) {
        int di = base + i;

        s_descs[di].desc       = &group->descriptors[i];
        s_descs[di].grp_idx    = (uint8_t)gi;
        s_descs[di].state      = CLAW_CAP_STATE_LOADED;
        s_descs[di].init_called = false;
        s_descs[di].active_calls = 0;
        s_descs[di].live       = true;

        if (!ht_insert_name((uint16_t)di) ||
            !ht_insert_id((uint16_t)di)) {
            /* Hash tables full — undo */
            int j;

            for (j = 0; j <= i; j++) {
                int dj = base + j;

                ht_remove_name((uint16_t)dj);
                ht_remove_id((uint16_t)dj);
                _memset(&s_descs[dj], 0, sizeof(s_descs[dj]));
            }
            _memset(&s_groups[gi], 0, sizeof(s_groups[gi]));
            reg_give();
            return RTK_ERR_NOMEM;
        }
    }

    reg_give();

    /* If the system has already started, immediately start this group */
    if (s_started) {
        rc = claw_cap_enable_group(group->group_id);
        if (rc != RTK_SUCCESS) {
            RTK_LOGW(TAG, "enable failed for new group %s: %s\n",
                     group->group_id, rtk_err_to_name(rc));
        }
    }

    RTK_LOGD(TAG, "group '%s' registered (%u caps)\n",
             group->group_id, (unsigned)group->descriptor_count);
    return RTK_SUCCESS;
}

int claw_cap_start_all(void)
{
    int i;

    if (!s_initialized) {
        return RTK_FAIL;
    }
    if (s_started) {
        return RTK_SUCCESS;
    }
    s_started = true;

    for (i = 0; i < CAP_MAX_GROUPS; i++) {
        if (!s_groups[i].live) {
            continue;
        }
        if (s_groups[i].state == CLAW_CAP_STATE_INACTIVE) {
            continue;
        }
        if (claw_cap_enable_group(s_groups[i].group->group_id) != RTK_SUCCESS) {
            RTK_LOGW(TAG, "start failed for group '%s'\n",
                     s_groups[i].group->group_id);
        }
    }
    return RTK_SUCCESS;
}

int claw_cap_stop_all(void)
{
    int first_rc = RTK_SUCCESS;
    int i;

    if (!s_initialized) {
        return RTK_FAIL;
    }

    for (i = 0; i < CAP_MAX_GROUPS; i++) {
        if (!s_groups[i].live ||
            s_groups[i].state != CLAW_CAP_STATE_ACTIVE) {
            continue;
        }
        if (claw_cap_disable_group(s_groups[i].group->group_id) != RTK_SUCCESS
            && first_rc == RTK_SUCCESS) {
            first_rc = RTK_FAIL;
        }
    }
    s_started = false;
    return first_rc;
}

int claw_cap_enable_group(const char *group_id)
{
    int gi;
    int rc;

    if (!s_initialized) {
        return RTK_FAIL;
    }
    if (!group_id || !group_id[0]) {
        return RTK_ERR_BADARG;
    }

    reg_take();
    gi = reg_find_group(group_id);
    if (gi < 0) {
        reg_give();
        return RTK_FAIL;
    }

    if (s_groups[gi].state == CLAW_CAP_STATE_STOPPING ||
        s_groups[gi].state == CLAW_CAP_STATE_REMOVING) {
        reg_give();
        return RTK_FAIL;
    }

    if (!s_started) {
        /* System not started yet — just mark enabled */
        if (s_groups[gi].state == CLAW_CAP_STATE_INACTIVE) {
            grp_set_state(gi, CLAW_CAP_STATE_LOADED);
        }
        reg_give();
        return RTK_SUCCESS;
    }

    if (s_groups[gi].state == CLAW_CAP_STATE_ACTIVE) {
        reg_give();
        return RTK_SUCCESS;
    }

    /* Transition to REGISTERED first so callbacks see a stable state */
    grp_set_state(gi, CLAW_CAP_STATE_LOADED);
    reg_give();

    rc = life_start_group(gi);

    reg_take();
    if (gi < CAP_MAX_GROUPS && s_groups[gi].live) {
        if (rc == RTK_SUCCESS) {
            grp_set_state(gi, CLAW_CAP_STATE_ACTIVE);
        } else {
            grp_set_state(gi, CLAW_CAP_STATE_INACTIVE);
        }
    }
    reg_give();

    if (rc != RTK_SUCCESS) {
        RTK_LOGW(TAG, "capability not found: %s\n", group_id);
    }
    return rc;
}

int claw_cap_disable_group(const char *group_id)
{
    int gi;
    int rc;

    if (!s_initialized) {
        return RTK_FAIL;
    }
    if (!group_id || !group_id[0]) {
        return RTK_ERR_BADARG;
    }

    reg_take();
    gi = reg_find_group(group_id);
    if (gi < 0) {
        reg_give();
        return RTK_FAIL;
    }

    if (s_groups[gi].state == CLAW_CAP_STATE_INACTIVE) {
        reg_give();
        return RTK_SUCCESS;
    }
    if (s_groups[gi].state == CLAW_CAP_STATE_STOPPING ||
        s_groups[gi].state == CLAW_CAP_STATE_REMOVING) {
        reg_give();
        return RTK_FAIL;
    }

    grp_set_state(gi, CLAW_CAP_STATE_INACTIVE);
    reg_give();

    rc = life_stop_group(gi);
    if (rc != RTK_SUCCESS) {
        RTK_LOGW(TAG, "capability '%s' is unavailable\n", group_id);
    }
    return RTK_SUCCESS;
}

int claw_cap_unregister_group(const char *group_id, uint32_t timeout_ms)
{
    int         gi;
    uint32_t    deadline;
    int         stop_rc;

    if (!s_initialized) {
        return RTK_FAIL;
    }
    if (!group_id || !group_id[0]) {
        return RTK_ERR_BADARG;
    }

    deadline = rtos_time_get_current_system_time_ms() +
               ((timeout_ms == UINT32_MAX) ? 0xFFFFFFFFUL : timeout_ms);

    reg_take();
    gi = reg_find_group(group_id);
    if (gi < 0) {
        reg_give();
        return RTK_FAIL;
    }
    if (s_groups[gi].state == CLAW_CAP_STATE_REMOVING) {
        reg_give();
        return RTK_FAIL;
    }

    /* Mark draining — new calls will be rejected */
    grp_set_state(gi, CLAW_CAP_STATE_STOPPING);
    reg_give();

    /* Drain: wait for active_calls to reach zero */
    for (;;) {
        bool busy;

        reg_take();
        busy = grp_any_inflight(gi);
        if (!busy) {
            grp_set_state(gi, CLAW_CAP_STATE_REMOVING);
            reg_give();
            break;
        }
        reg_give();

        if (timeout_ms != UINT32_MAX &&
            rtos_time_get_current_system_time_ms() >= deadline) {
            return RTK_ERR_TIMEOUT;
        }
        rtos_time_delay_ms(CAP_UNLOAD_TICK_MS);
    }

    stop_rc = life_stop_group(gi);

    reg_take();
    reg_free_group(gi);
    reg_give();

    RTK_LOGI(TAG, "group '%s' unregistered\n", group_id);
    return stop_rc;
}

int claw_cap_unregister(const char *id_or_name, uint32_t timeout_ms)
{
    int   di;
    int   gi;
    const char *gid;

    if (!s_initialized) {
        return RTK_FAIL;
    }
    if (!id_or_name || !id_or_name[0]) {
        return RTK_ERR_BADARG;
    }

    reg_take();
    di = reg_find_desc(id_or_name);
    if (di < 0) {
        reg_give();
        return RTK_FAIL;
    }
    gi  = s_descs[di].grp_idx;
    gid = (s_groups[gi].live && s_groups[gi].group)
          ? s_groups[gi].group->group_id
          : NULL;
    /* Only allow single-descriptor groups to be unregistered this way */
    if (!gid || s_groups[gi].desc_count != 1) {
        reg_give();
        return RTK_FAIL;
    }
    reg_give();

    return claw_cap_unregister_group(gid, timeout_ms);
}

int claw_cap_set_llm_visible_groups(const char *const *group_ids, size_t count)
{
    if (!s_initialized) {
        return RTK_FAIL;
    }
    if (count > 0 && !group_ids) {
        return RTK_ERR_BADARG;
    }
    if (count > CAP_VIS_MAX) {
        return RTK_ERR_BADARG;
    }

    vis_set_global(group_ids, count);
    RTK_LOGI(TAG, "set %u LLM-visible groups\n", (unsigned)count);
    return RTK_SUCCESS;
}

int claw_cap_set_session_llm_visible_groups(const char *session_id,
                                            const char *const *group_ids,
                                            size_t count)
{
    if (!s_initialized) {
        return RTK_FAIL;
    }
    if (!session_id || !session_id[0]) {
        return RTK_ERR_BADARG;
    }
    if (count > 0 && !group_ids) {
        return RTK_ERR_BADARG;
    }

    return vis_scope_set(session_id, group_ids, count);
}

bool claw_cap_group_exists(const char *group_id)
{
    bool found;

    if (!s_initialized || !group_id || !group_id[0]) {
        return false;
    }
    reg_take();
    found = reg_find_group(group_id) >= 0;
    reg_give();
    return found;
}

int claw_cap_get_group_state(const char *group_id, claw_cap_state_t *state)
{
    int gi;

    if (!s_initialized || !group_id || !state) {
        return RTK_ERR_BADARG;
    }

    reg_take();
    gi = reg_find_group(group_id);
    if (gi < 0) {
        reg_give();
        return RTK_FAIL;
    }
    *state = s_groups[gi].state;
    reg_give();
    return RTK_SUCCESS;
}

int claw_cap_get_descriptor_state(const char *id_or_name,
                                  claw_cap_descriptor_info_t *info)
{
    int di, gi;

    if (!s_initialized || !id_or_name || !info) {
        return RTK_ERR_BADARG;
    }

    reg_take();
    di = reg_find_desc(id_or_name);
    if (di < 0) {
        reg_give();
        return RTK_FAIL;
    }

    gi = s_descs[di].grp_idx;
    info->id           = s_descs[di].desc->id;
    info->name         = s_descs[di].desc->name;
    info->group_id     = (s_groups[gi].live && s_groups[gi].group)
                         ? s_groups[gi].group->group_id : NULL;
    info->state        = s_descs[di].state;
    info->active_calls = s_descs[di].active_calls;
    reg_give();
    return RTK_SUCCESS;
}

const claw_cap_descriptor_t *claw_cap_find(const char *id_or_name)
{
    const claw_cap_descriptor_t *result = NULL;
    int di;

    if (!s_initialized || !id_or_name || !id_or_name[0]) {
        return NULL;
    }

    reg_take();
    di = reg_find_desc(id_or_name);
    if (di >= 0 && desc_is_ready(&s_descs[di])) {
        result = s_descs[di].desc;
    }
    reg_give();
    return result;
}

claw_cap_list_t claw_cap_list(void)
{
    claw_cap_list_t lst = {0};
    size_t cnt = 0;
    int    i;

    if (!s_initialized) {
        return lst;
    }

    reg_take();
    for (i = 0; i < CAP_MAX_DESCS; i++) {
        if (!desc_is_ready(&s_descs[i])) {
            continue;
        }
        s_desc_snap[cnt++] = *s_descs[i].desc;
    }
    reg_give();

    lst.items = s_desc_snap;
    lst.count = cnt;
    return lst;
}

claw_cap_group_list_t claw_cap_list_groups(void)
{
    claw_cap_group_list_t lst = {0};
    size_t cnt = 0;
    int    i;

    if (!s_initialized) {
        return lst;
    }

    reg_take();
    for (i = 0; i < CAP_MAX_GROUPS; i++) {
        group_entry_t *ge = &s_groups[i];

        if (!ge->live || !ge->group) {
            continue;
        }
        s_grp_snap[cnt].group_id        = ge->group->group_id;
        s_grp_snap[cnt].plugin_name     = ge->group->plugin_name;
        s_grp_snap[cnt].version         = ge->group->version;
        s_grp_snap[cnt].state           = ge->state;
        s_grp_snap[cnt].descriptor_count = ge->desc_count;
        cnt++;
    }
    reg_give();

    lst.items = s_grp_snap;
    lst.count = cnt;
    return lst;
}

int claw_cap_call(const char *id_or_name,
                  const char *input_json,
                  const claw_cap_call_context_t *ctx,
                  char **output)
{
    claw_cap_execute_fn exec  = NULL;
    const char         *name  = NULL;
    int                 di;
    int                 rc;
    const char         *sid;

    if (!output) {
        return RTK_ERR_BADARG;
    }
    *output = NULL;

    if (!s_initialized) {
        *output = strdup(id_or_name
                         ? "capability not found"
                         : "capability registry not initialized");
        return RTK_FAIL;
    }

    sid = (ctx &&
           ctx->caller == CLAW_CAP_CALLER_LLM &&
           ctx->session_id &&
           ctx->session_id[0])
          ? ctx->session_id : NULL;

    reg_take();
    di = reg_find_desc(id_or_name);
    if (di < 0) {
        reg_give();
        {
            int n = DiagSnPrintf(NULL, 0, "capability not found: %s",
                             id_or_name ? id_or_name : "");
            char *msg = malloc((size_t)n + 1);
            if (msg) DiagSnPrintf(msg, (size_t)n + 1, "capability not found: %s",
                              id_or_name ? id_or_name : "");
            *output = msg;
        }
        return RTK_FAIL;
    }

    {
        desc_entry_t *de = &s_descs[di];

        if (!desc_is_ready(de) || !de->desc->execute) {
            reg_give();
            {
                int n = DiagSnPrintf(NULL, 0, "capability '%s' is unavailable",
                                 id_or_name ? id_or_name : "");
                char *msg = malloc((size_t)n + 1);
                if (msg) DiagSnPrintf(msg, (size_t)n + 1, "capability '%s' is unavailable",
                                  id_or_name ? id_or_name : "");
                *output = msg;
            }
            return RTK_FAIL;
        }

        if (ctx && ctx->caller == CLAW_CAP_CALLER_LLM &&
            !vis_check_desc(de, sid)) {
            const char *dname = de->desc->name;
            reg_give();
            {
                int n = DiagSnPrintf(NULL, 0, "capability '%s' not accessible to LLM",
                                 dname ? dname : "");
                char *msg = malloc((size_t)n + 1);
                if (msg) DiagSnPrintf(msg, (size_t)n + 1,
                                  "capability '%s' not accessible to LLM",
                                  dname ? dname : "");
                *output = msg;
            }
            return RTK_FAIL;
        }

        /* Increment active_calls before releasing lock */
        de->active_calls++;
        exec = de->desc->execute;
        name = de->desc->name;
    }
    reg_give();

    /* Execute outside lock */
    rc = exec(input_json ? input_json : "{}", ctx, output);

    /* Decrement active_calls */
    reg_take();
    if (di < CAP_MAX_DESCS &&
        s_descs[di].live &&
        s_descs[di].active_calls > 0) {
        s_descs[di].active_calls--;
    }
    reg_give();

    if (rc != RTK_SUCCESS && (!*output || (*output)[0] == '\0')) {
        free(*output);
        int n = DiagSnPrintf(NULL, 0, "capability '%s' execution error: %s",
                         name ? name : "?", rtk_err_to_name(rc));
        char *msg = malloc((size_t)n + 1);
        if (msg) DiagSnPrintf(msg, (size_t)n + 1, "capability '%s' execution error: %s",
                          name ? name : "?", rtk_err_to_name(rc));
        *output = msg;
    }
    return rc;
}

char *claw_cap_build_llm_tools_json(const claw_cap_call_context_t *ctx,
                                    bool wrap_for_responses_api)
{
    if (!s_initialized) {
        return NULL;
    }
    return json_build_llm_tools(ctx, wrap_for_responses_api);
}

char *claw_cap_build_catalog(void)
{
    if (!s_initialized) {
        return NULL;
    }
    return json_build_catalog();
}

const char *claw_cap_state_to_string(claw_cap_state_t state)
{
    switch (state) {
    case CLAW_CAP_STATE_LOADED: return "loaded";
    case CLAW_CAP_STATE_ACTIVE:    return "active";
    case CLAW_CAP_STATE_INACTIVE:   return "inactive";
    case CLAW_CAP_STATE_STOPPING:   return "stopping";
    case CLAW_CAP_STATE_REMOVING:  return "removing";
    default:                        return "unknown";
    }
}
