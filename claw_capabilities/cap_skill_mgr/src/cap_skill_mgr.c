/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_skill_mgr.h"
#include "claw_cap.h"
#include "claw_cap_registry.h"
#include "claw_agent.h"
#include "claw_config.h"
#include <cJSON.h>
#include "platform_stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Platform dirent + opendir/readdir/closedir support via vfs.h */
#include "vfs.h"
#include "cap_lua.h"

/* Lua execution lives in cap_lua (improvement #12 Inc 3). This module is pure
 * skill management: it no longer creates lua_States or runs scripts. Scripts
 * are executed by LLM/AT via the separate lua_run(path) tool. */

#define TAG "cap_skill_mgr"

/* Max SKILL.md size returned by activate_skill */
#define SKILL_MD_MAX_SIZE 8192
/* Max number of concurrently active skills (per session) */
#define MAX_ACTIVE_SKILLS 8
/* Max cap_groups in a single skill's frontmatter */
#define SKILL_MAX_CAP_GROUPS 8
/* Max cap_groups surfaced per session (must not exceed claw_cap CAP_SESSION_GIDS) */
#define SESSION_MAX_VISIBLE_GROUPS 8
/* Max length of a cap_group id */
#define CAP_GROUP_ID_MAX 64

/* Read-only built-in skills live in the littlefs region mounted at "rolfs:"
 * (improvement #12 Inc 2). They are real files packed into the app image, not
 * C strings — see tools/image_scripts/rolfs_content/skills/. User skills live in
 * the writable VFS directory configured at init (s_rt.skills_dir). */
#define ROLFS_SKILLS_DIR "rolfs:/skills"

/* Per-session active-skill lists live here (improvement #12 Inc 6, gap S5).
 * The filename is "sk_<sanitized>_<fnv1a-hash>.skills.json"; the "sk_" prefix
 * keeps these distinct from claw_memory's "s_*.json" history files so that
 * claw_memory_clear_all_sessions() (which only removes "s_*.json") never
 * touches them. */
#define SESSION_SKILLS_DIR  "vfs:/session"

/* Default session id used when a call arrives without one (e.g. internal /
 * manual callers). Keeps behaviour deterministic and isolated from real
 * channel sessions. */
#define DEFAULT_SESSION_ID  "_default"

/* Gateable capability groups (improvement #12 Inc 6, gap S1).
 *
 * Visibility in claw_cap is allow-list based: when the global visible list is
 * non-empty, a group is LLM-visible only if it is in the global list OR in the
 * caller's per-session scope. We therefore set the GLOBAL list to "every
 * registered group EXCEPT the gateable ones" (see install_global_base_groups),
 * so the gateable groups below start hidden for every session and only become
 * visible for a session once a skill that declares them is activated.
 *
 * Only peripheral / hardware-domain groups belong here — infra/management
 * groups (skill_mgr, lua, files, ...) are always visible. */
static const char *const GATEABLE_GROUPS[] = {
    "audio_stream",
    /* improvement #12 Inc 8 (gap S7): the board-inspection tools
     * (board_list_devices / board_get_device / board_query_peripheral) stay
     * hidden until the "board_hardware_info" skill is activated, so the LLM is
     * pushed to consult the real on-board peripherals before running any
     * hardware script. The compact board summary context provider still runs
     * unconditionally, so the model is never blind to the board name. */
    "board",
    /* Event-dispatcher rule management (router_mgr). The rule CRUD tools stay
     * hidden until the "rule_automation" skill is activated, so the LLM only
     * sees them (and the rule-authoring guide the skill injects) when a task
     * actually calls for a declarative fast-path rule — keeping the default
     * tool list lean. cap_router_mgr registers at a high order and is not in the
     * base-visibility snapshot, so it is hidden by default regardless; listing
     * it here is what lets skill activation surface it into a session's scope. */
    "router_mgr",
};
#define GATEABLE_GROUPS_COUNT (sizeof(GATEABLE_GROUPS) / sizeof(GATEABLE_GROUPS[0]))

static bool group_is_gateable(const char *gid)
{
    for (size_t i = 0; i < GATEABLE_GROUPS_COUNT; i++) {
        if (strcmp(GATEABLE_GROUPS[i], gid) == 0) return true;
    }
    return false;
}

bool cap_skill_mgr_group_is_hidden(const char *gid, const claw_cap_visibility_config_t *vis)
{
    for (uint8_t i = 0; i < vis->hidden_count; i++) {
        if (strcmp(gid, vis->hidden[i]) == 0) return true;
    }
    return false;
}
/* Keep the old static name as an alias for internal callers. */
#define group_is_hidden cap_skill_mgr_group_is_hidden

/* Per-session gating restoration tracking.
 * On device boot, cap_groups visibility is reset to base visibility; each
 * session's skill-gated groups must be re-applied on the first request.
 * After that, gating is stable for the device lifetime — no need to re-sync
 * on every request. We track which sessions have been restored using a small
 * ring buffer of session_id strings. */
#define GATING_RESTORED_MAX 16

static struct {
    char skills_dir[64];   /* writable user skills dir, e.g. "vfs:/skills" */
    char gating_restored[GATING_RESTORED_MAX][96];
    int  gating_restored_count;
} s_rt;

static int gating_is_restored(const char *session_id)
{
    if (!session_id || !session_id[0]) return 0;
    for (int i = 0; i < s_rt.gating_restored_count; i++) {
        if (strcmp(s_rt.gating_restored[i], session_id) == 0) return 1;
    }
    return 0;
}

static void gating_mark_restored(const char *session_id)
{
    if (!session_id || !session_id[0]) return;
    if (gating_is_restored(session_id)) return;
    if (s_rt.gating_restored_count >= GATING_RESTORED_MAX) {
        /* Ring is full — reset entirely; next request for any session will
         * re-sync once and re-populate. This is safe: worst case is one
         * extra sync per session on the next request. */
        s_rt.gating_restored_count = 0;
    }
    strlcpy(s_rt.gating_restored[s_rt.gating_restored_count],
            session_id,
            sizeof(s_rt.gating_restored[0]));
    s_rt.gating_restored_count++;
}

/* ---- Active skills persistence (per-session, improvement #12 Inc 6) ---- */

/* Build the per-session active-skills file path.
 *
 * "sk_<sanitized>_<fnv1a-hash>.skills.json": the sanitized prefix keeps the
 * name human-readable, the FNV-1a hash of the *original* id disambiguates ids
 * that sanitize to the same prefix, and ".skills.json" + "sk_" prefix keep it
 * distinct from claw_memory's "s_*.json" history files. */
static void session_skills_path(const char *session_id, char *buf, size_t n)
{
    const char *sid = (session_id && session_id[0]) ? session_id : DEFAULT_SESSION_ID;

    /* sanitize: keep [a-zA-Z0-9_-], replace the rest with '_', cap length */
    char sanitized[33];
    size_t i = 0;
    for (const char *p = sid; *p && i < sizeof(sanitized) - 1; p++, i++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            sanitized[i] = c;
        } else {
            sanitized[i] = '_';
        }
    }
    sanitized[i] = '\0';

    /* FNV-1a 32-bit hash of the original (unsanitized) session id */
    uint32_t hash = 2166136261u;        /* FNV offset basis */
    for (const char *p = sid; *p; p++) {
        hash ^= (uint32_t)(unsigned char)(*p);
        hash *= 16777619u;              /* FNV prime */
    }

    DiagSnPrintf(buf, n, "%s/sk_%s_%08lx.skills.json",
                 SESSION_SKILLS_DIR, sanitized, (unsigned long)hash);
}

/* Load active skills list for a session. Returns cJSON array (caller deletes). */
static cJSON *load_active_skills(const char *session_id)
{
    char path[160];
    session_skills_path(session_id, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return cJSON_CreateArray();
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return cJSON_CreateArray(); }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return cJSON_CreateArray(); }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return cJSON_CreateArray(); }
    return arr;
}

static void save_active_skills(const char *session_id, const cJSON *arr)
{
    char path[160];
    session_skills_path(session_id, path, sizeof(path));
    /* When the list is empty, remove the file so a session leaves no residue. */
    if (cJSON_GetArraySize((cJSON *)arr) == 0) {
        remove(path);
        return;
    }
    char *s = cJSON_PrintUnformatted(arr);
    if (!s) return;
    mkdir(SESSION_SKILLS_DIR, 0777);
    FILE *f = fopen(path, "w");
    if (f) { fwrite(s, 1, strlen(s), f); fclose(f); }
    free(s);
}

static int active_skills_contains(const cJSON *arr, const char *name)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (cJSON_IsString(item) && strcmp(item->valuestring, name) == 0) return 1;
    }
    return 0;
}

static void active_skills_add(cJSON *arr, const char *name)
{
    if (active_skills_contains(arr, name)) return;
    if (cJSON_GetArraySize(arr) >= MAX_ACTIVE_SKILLS) {
        cJSON_DeleteItemFromArray(arr, 0); /* evict oldest */
    }
    cJSON_AddItemToArray(arr, cJSON_CreateString(name));
}

static void active_skills_remove(cJSON *arr, const char *name)
{
    int i = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (cJSON_IsString(item) && strcmp(item->valuestring, name) == 0) {
            cJSON_DeleteItemFromArray(arr, i);
            return;
        }
        i++;
    }
}

/* ---- Helpers ---- */

/* Validate that a skill name is safe to use as a filesystem path component.
 * Accepts only [a-zA-Z0-9_-] — rejects '/', '..', null bytes, and all other
 * characters that could escape the skills directory. */
#define SKILL_NAME_MAX 64   /* keeps DiagSnPrintf paths within all 192-byte buffers */

static bool skill_name_is_safe(const char *name)
{
    if (!name || name[0] == '\0') return false;
    size_t len = 0;
    for (const char *p = name; *p; p++, len++) {
        if (len >= SKILL_NAME_MAX) return false;
        char c = *p;
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/* <base_dir>/<name>/SKILL.md for an arbitrary base (rolfs: or vfs:). */
static void build_md_path_in(char *buf, size_t buf_size, const char *base_dir, const char *name)
{
    DiagSnPrintf(buf, buf_size, "%s/%s/SKILL.md", base_dir, name);
}

/* Returns 1 if file exists */
static int file_exists(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* Read file into malloc'd buffer (caller frees). Returns NULL on error. */
static char *read_file_alloc(const char *path, size_t max_size)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    size_t read_sz = (size_t)sz < max_size ? (size_t)sz : max_size;
    char *buf = malloc(read_sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, read_sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Resolve a skill name to its base directory: built-in (rolfs:) takes precedence
 * over a same-named user skill. Returns ROLFS_SKILLS_DIR / s_rt.skills_dir, or
 * NULL if no SKILL.md exists in either source. */
static const char *resolve_skill_base(const char *name)
{
    char md_path[192];
    build_md_path_in(md_path, sizeof(md_path), ROLFS_SKILLS_DIR, name);
    if (file_exists(md_path)) return ROLFS_SKILLS_DIR;
    build_md_path_in(md_path, sizeof(md_path), s_rt.skills_dir, name);
    if (file_exists(md_path)) return s_rt.skills_dir;
    return NULL;
}

/* Expand all "{CUR_SKILL_DIR}" tokens in `doc` to the skill's absolute
 * directory "<base>/<name>". Returns a newly malloc'd string (caller frees);
 * on OOM returns NULL. If the token is absent, returns a plain copy. */
static char *expand_cur_skill_dir(const char *doc, const char *base, const char *name)
{
    static const char TOKEN[] = "{CUR_SKILL_DIR}";
    const size_t tok_len = sizeof(TOKEN) - 1;

    char dir[192];
    DiagSnPrintf(dir, sizeof(dir), "%s/%s", base, name);
    size_t dir_len = strlen(dir);

    /* Count occurrences to size the output buffer. */
    size_t count = 0;
    for (const char *s = doc; (s = strstr(s, TOKEN)) != NULL; s += tok_len) count++;

    size_t doc_len = strlen(doc);
    size_t out_len = doc_len + count * (dir_len > tok_len ? dir_len - tok_len : 0) + 1;
    char *out = malloc(out_len);
    if (!out) return NULL;

    char *w = out;
    const char *r = doc;
    const char *hit;
    while ((hit = strstr(r, TOKEN)) != NULL) {
        size_t chunk = (size_t)(hit - r);
        _memcpy(w, r, chunk); w += chunk;
        _memcpy(w, dir, dir_len); w += dir_len;
        r = hit + tok_len;
    }
    strcpy(w, r);
    return out;
}

/* ---- SKILL.md frontmatter parsing (improvement #12 Inc 6, gap S2) ----
 *
 * A skill document MAY begin with a YAML frontmatter block delimited by lines
 * containing only "---":
 *
 *     ---
 *     name: <id>
 *     description: "..."
 *     compatibility: RTL8721F
 *     metadata:
 *       cap_groups: group_a group_b   (space/comma-separated, or [a, b])
 *       manage_mode: readonly         (or: editable)
 *       category: authoring           (documentation-only, ignored by parser)
 *     ---
 *     <markdown body...>
 *
 * Legacy JSON format (first non-blank char '{') is still accepted for backwards
 * compatibility but all current skills use YAML. Format is auto-detected.
 *
 * Tolerances: a leading UTF-8 BOM, and CRLF ("\r\n") line endings. The parser
 * is hand-written and strict: a malformed frontmatter
 * makes the skill invalid (rejected from the catalog, error logged) rather
 * than silently accepted. */

typedef enum {
    SKILL_MODE_READONLY = 0,
    SKILL_MODE_RUNTIME  = 1,
} skill_manage_mode_t;

#define SKILL_DESC_MAX 192

typedef struct {
    bool                valid;
    bool                has_frontmatter;
    char                name[SKILL_NAME_MAX + 1];
    char                description[SKILL_DESC_MAX + 1];
    skill_manage_mode_t manage_mode;
    char                cap_groups[SKILL_MAX_CAP_GROUPS][CAP_GROUP_ID_MAX];
    int                 cap_group_count;
} skill_meta_t;

/* Skip a leading UTF-8 BOM (EF BB BF) if present. */
static const char *skip_bom(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) return s + 3;
    return s;
}

/* Match a "---" delimiter line at *pp (tolerating a trailing '\r'); on match,
 * advance *pp past the line terminator and return true. */
static bool match_dashes_line(const char **pp)
{
    const char *p = *pp;
    if (p[0] != '-' || p[1] != '-' || p[2] != '-') return false;
    p += 3;
    if (*p == '\r') p++;
    if (*p != '\n') return false;
    p++;
    *pp = p;
    return true;
}

/* ---- YAML frontmatter helpers (agentskills.io YAML format support) ----
 *
 * Supports a minimal subset of YAML:
 *   name: value            (unquoted, "double-quoted", or 'single-quoted')
 *   metadata:              (introduces an indented sub-block)
 *     subkey: value
 *   cap_groups: a b        (space/comma-separated tokens, or [a, b] inline array)
 *
 * No external library required. Works on a malloc'd copy of the block. */

/* Parse a YAML scalar value from `src` (everything after ':') into dst.
 * Strips leading whitespace. Handles "double-quoted", 'single-quoted', and
 * plain unquoted (inline '#' comment stripped). Returns true on success. */
static bool yaml_parse_scalar(const char *src, char *dst, size_t dst_size)
{
    while (*src == ' ' || *src == '\t') src++;
    if (!*src) { dst[0] = '\0'; return true; }
    if (src[0] == '"') {
        src++;
        const char *end = strchr(src, '"');
        if (!end) return false;
        size_t n = (size_t)(end - src);
        if (n >= dst_size) n = dst_size - 1;
        _memcpy(dst, src, n);
        dst[n] = '\0';
        return true;
    }
    if (src[0] == '\'') {
        src++;
        const char *end = strchr(src, '\'');
        if (!end) return false;
        size_t n = (size_t)(end - src);
        if (n >= dst_size) n = dst_size - 1;
        _memcpy(dst, src, n);
        dst[n] = '\0';
        return true;
    }
    /* unquoted: strip trailing whitespace/CRLF and inline comment */
    size_t len = strlen(src);
    while (len > 0 && (src[len-1] == ' ' || src[len-1] == '\t' ||
                       src[len-1] == '\r' || src[len-1] == '\n')) len--;
    for (size_t i = 1; i < len; i++) {
        if (src[i] == '#' && (src[i-1] == ' ' || src[i-1] == '\t')) {
            len = i;
            while (len > 0 && (src[len-1] == ' ' || src[len-1] == '\t')) len--;
            break;
        }
    }
    if (len >= dst_size) len = dst_size - 1;
    _memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

/* Parse cap_groups value (space/comma separated or "[a, b]" inline array)
 * into out->cap_groups[]. Works on a mutable copy. Returns false on error. */
static bool yaml_parse_cap_groups(const char *src, skill_meta_t *out)
{
    char buf[128];
    while (*src == ' ' || *src == '\t') src++;
    size_t src_len = strlen(src);
    while (src_len > 0 && (src[src_len-1] == ' ' || src[src_len-1] == '\t' ||
                            src[src_len-1] == '\r' || src[src_len-1] == '\n')) src_len--;
    if (src_len == 0) return true;
    if (src_len >= sizeof(buf)) src_len = sizeof(buf) - 1;
    _memcpy(buf, src, src_len);
    buf[src_len] = '\0';
    char *p = buf;
    if (*p == '[') { p++; char *rb = strrchr(p, ']'); if (rb) *rb = '\0'; }
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        char *end = p;
        while (*end && *end != ' ' && *end != ',' && *end != '\t') end++;
        char saved = *end; *end = '\0';
        size_t tlen = strlen(p);
        while (tlen > 0 && (p[tlen-1] == ' ' || p[tlen-1] == '\t')) { p[tlen-1] = '\0'; tlen--; }
        if (tlen > 0) {
            if (!skill_name_is_safe(p)) return false;
            bool dup = false;
            for (int k = 0; k < out->cap_group_count; k++) {
                if (strcmp(out->cap_groups[k], p) == 0) { dup = true; break; }
            }
            if (!dup) {
                if (out->cap_group_count >= SKILL_MAX_CAP_GROUPS) return false;
                strlcpy(out->cap_groups[out->cap_group_count], p, sizeof(out->cap_groups[0]));
                out->cap_group_count++;
            }
        }
        p = end + (saved ? 1 : 0);
    }
    return true;
}

/* Parse YAML frontmatter content between the two --- delimiters.
 * `out` must be zero-initialized with manage_mode=READONLY before call.
 * Returns true on success. */
static bool parse_yaml_frontmatter_body(const char *yaml, size_t yaml_len,
                                        const char *expected_name, skill_meta_t *out)
{
    char *buf = malloc(yaml_len + 1);
    if (!buf) return false;
    _memcpy(buf, yaml, yaml_len);
    buf[yaml_len] = '\0';

    bool in_metadata = false;
    bool got_name    = false;
    bool got_desc    = false;
    bool ok          = true;
    char *line       = buf;
    char *buf_end    = buf + yaml_len;

    while (line < buf_end && ok) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        size_t llen = strlen(line);
        if (llen > 0 && line[llen-1] == '\r') line[llen-1] = '\0';

        int indent = 0;
        const char *p = line;
        while (*p == ' ' || *p == '\t') { indent++; p++; }
        if (!*p || *p == '#') { line = nl ? nl + 1 : buf_end; continue; }

        const char *colon = strchr(p, ':');
        if (!colon) { line = nl ? nl + 1 : buf_end; continue; }

        size_t key_len = (size_t)(colon - p);
        while (key_len > 0 && (p[key_len-1] == ' ' || p[key_len-1] == '\t')) key_len--;
        char key[64];
        if (key_len == 0 || key_len >= sizeof(key)) { line = nl ? nl + 1 : buf_end; continue; }
        _memcpy(key, p, key_len);
        key[key_len] = '\0';

        const char *val_raw = colon + 1;

        if (indent == 0) {
            if (strcmp(key, "name") == 0) {
                char val[SKILL_NAME_MAX + 1];
                if (!yaml_parse_scalar(val_raw, val, sizeof(val))) { ok = false; break; }
                if (!skill_name_is_safe(val)) {
                    RTK_LOGW(TAG, "skill '%s': YAML name '%s' illegal chars\n", expected_name, val);
                    ok = false; break;
                }
                if (expected_name && val[0] && strcmp(val, expected_name) != 0) {
                    RTK_LOGW(TAG, "skill '%s': YAML name '%s' != dir\n", expected_name, val);
                    ok = false; break;
                }
                strlcpy(out->name, val, sizeof(out->name));
                got_name = true; in_metadata = false;
            } else if (strcmp(key, "description") == 0) {
                char val[SKILL_DESC_MAX + 1];
                if (!yaml_parse_scalar(val_raw, val, sizeof(val)) || !val[0]) {
                    RTK_LOGW(TAG, "skill '%s': YAML description missing\n", expected_name);
                    ok = false; break;
                }
                strlcpy(out->description, val, sizeof(out->description));
                got_desc = true; in_metadata = false;
            } else if (strcmp(key, "metadata") == 0) {
                in_metadata = true;
            } else {
                in_metadata = false;
            }
        } else if (indent >= 2 && in_metadata) {
            if (strcmp(key, "manage_mode") == 0) {
                char val[32];
                yaml_parse_scalar(val_raw, val, sizeof(val));
                if (strcmp(val, "runtime") == 0) {
                    out->manage_mode = SKILL_MODE_RUNTIME;
                } else if (strcmp(val, "readonly") == 0 || val[0] == '\0') {
                    out->manage_mode = SKILL_MODE_READONLY;
                } else {
                    /* Unknown manage_mode: silently fall back to readonly for
                     * forward compatibility (e.g. LLM writes "user", "standard"). */
                    RTK_LOGD(TAG, "skill '%s': unknown manage_mode '%s', defaulting to readonly\n",
                             expected_name, val);
                    out->manage_mode = SKILL_MODE_READONLY;
                }
            } else if (strcmp(key, "cap_groups") == 0) {
                if (!yaml_parse_cap_groups(val_raw, out)) {
                    RTK_LOGW(TAG, "skill '%s': YAML invalid cap_groups\n", expected_name);
                    ok = false; break;
                }
            }
            /* category, prerequisites, peripherals: documentation-only, ignored */
        }
        line = nl ? nl + 1 : buf_end;
    }
    free(buf);
    if (!ok) return false;
    if (!got_name || !out->name[0]) {
        RTK_LOGW(TAG, "skill '%s': YAML frontmatter missing 'name'\n", expected_name);
        return false;
    }
    if (!got_desc || !out->description[0]) {
        RTK_LOGW(TAG, "skill '%s': YAML frontmatter missing 'description'\n", expected_name);
        return false;
    }
    return true;
}

/* Parse a skill document's optional frontmatter. `expected_name` is the
 * directory name the skill was loaded from; the frontmatter "name" must match
 * it. On a document with no frontmatter, returns valid=true,
 * has_frontmatter=false, manage_mode=readonly, no cap_groups (legacy plain
 * markdown stays usable). On malformed frontmatter, returns valid=false.
 * Format auto-detected: JSON (legacy, first non-blank char '{') or YAML. */
static void parse_skill_frontmatter(const char *doc, const char *expected_name,
                                    skill_meta_t *out)
{
    _memset(out, 0, sizeof(*out));
    out->manage_mode = SKILL_MODE_READONLY;
    strlcpy(out->name, expected_name ? expected_name : "", sizeof(out->name));

    const char *p = skip_bom(doc);
    if (!match_dashes_line(&p)) {
        /* No frontmatter — legacy plain markdown is accepted as-is. */
        out->valid = true;
        out->has_frontmatter = false;
        return;
    }

    /* Find the closing "---" line. Scan line by line. */
    const char *json_start = p;
    const char *json_end = NULL;
    const char *scan = p;
    while (*scan) {
        const char *line = scan;
        if (match_dashes_line(&scan)) {
            json_end = line;   /* delimiter line begins here */
            break;
        }
        /* advance to next line */
        const char *nl = strchr(scan, '\n');
        if (!nl) { scan += strlen(scan); break; }
        scan = nl + 1;
    }
    if (!json_end) {
        RTK_LOGW(TAG, "skill '%s': unterminated frontmatter\n", expected_name);
        out->valid = false;
        return;
    }

    size_t json_len = (size_t)(json_end - json_start);

    /* Auto-detect format: first non-blank char '{' → legacy JSON; else YAML. */
    const char *detect = json_start;
    while (detect < json_end && (*detect == ' ' || *detect == '\t' ||
                                  *detect == '\r' || *detect == '\n')) detect++;
    if (detect >= json_end || *detect != '{') {
        if (!parse_yaml_frontmatter_body(json_start, json_len, expected_name, out)) {
            out->valid = false;
            return;
        }
        out->valid = true;
        out->has_frontmatter = true;
        return;
    }

    /* Legacy JSON frontmatter */
    char *json = malloc(json_len + 1);
    if (!json) { out->valid = false; return; }
    _memcpy(json, json_start, json_len);
    json[json_len] = '\0';

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root || !cJSON_IsObject(root)) {
        RTK_LOGW(TAG, "skill '%s': frontmatter is not a JSON object\n", expected_name);
        cJSON_Delete(root);
        out->valid = false;
        return;
    }

    /* name (required, must equal directory name, only [a-z0-9_-]) */
    cJSON *jname = cJSON_GetObjectItem(root, "name");
    if (!jname || !cJSON_IsString(jname) || !jname->valuestring || !jname->valuestring[0]) {
        RTK_LOGW(TAG, "skill '%s': frontmatter missing 'name'\n", expected_name);
        cJSON_Delete(root);
        out->valid = false;
        return;
    }
    if (!skill_name_is_safe(jname->valuestring)) {
        RTK_LOGW(TAG, "skill '%s': frontmatter name '%s' has illegal chars\n",
                 expected_name, jname->valuestring);
        cJSON_Delete(root);
        out->valid = false;
        return;
    }
    if (expected_name && strcmp(jname->valuestring, expected_name) != 0) {
        RTK_LOGW(TAG, "skill '%s': frontmatter name '%s' != directory name\n",
                 expected_name, jname->valuestring);
        cJSON_Delete(root);
        out->valid = false;
        return;
    }
    strlcpy(out->name, jname->valuestring, sizeof(out->name));

    /* description (required) */
    cJSON *jdesc = cJSON_GetObjectItem(root, "description");
    if (!jdesc || !cJSON_IsString(jdesc) || !jdesc->valuestring || !jdesc->valuestring[0]) {
        RTK_LOGW(TAG, "skill '%s': frontmatter missing 'description'\n", expected_name);
        cJSON_Delete(root);
        out->valid = false;
        return;
    }
    strlcpy(out->description, jdesc->valuestring, sizeof(out->description));

    /* metadata (optional): cap_groups[], manage_mode */
    cJSON *jmeta = cJSON_GetObjectItem(root, "metadata");
    if (jmeta && cJSON_IsObject(jmeta)) {
        cJSON *jmode = cJSON_GetObjectItem(jmeta, "manage_mode");
        if (jmode && cJSON_IsString(jmode) && jmode->valuestring) {
            if (strcmp(jmode->valuestring, "runtime") == 0) {
                out->manage_mode = SKILL_MODE_RUNTIME;
            } else if (strcmp(jmode->valuestring, "readonly") == 0) {
                out->manage_mode = SKILL_MODE_READONLY;
            } else {
                /* Unknown manage_mode: silently fall back to readonly for
                 * forward compatibility (e.g. LLM writes "user", "standard"). */
                RTK_LOGD(TAG, "skill '%s': unknown manage_mode '%s', defaulting to readonly\n",
                         expected_name, jmode->valuestring);
                out->manage_mode = SKILL_MODE_READONLY;
            }
        }

        cJSON *jgroups = cJSON_GetObjectItem(jmeta, "cap_groups");
        if (jgroups) {
            if (!cJSON_IsArray(jgroups)) {
                RTK_LOGW(TAG, "skill '%s': cap_groups must be an array\n", expected_name);
                cJSON_Delete(root);
                out->valid = false;
                return;
            }
            cJSON *g;
            cJSON_ArrayForEach(g, jgroups) {
                if (!cJSON_IsString(g) || !g->valuestring || !g->valuestring[0]) {
                    RTK_LOGW(TAG, "skill '%s': cap_groups entry not a non-empty string\n",
                             expected_name);
                    cJSON_Delete(root);
                    out->valid = false;
                    return;
                }
                /* uniqueness */
                bool dup = false;
                for (int k = 0; k < out->cap_group_count; k++) {
                    if (strcmp(out->cap_groups[k], g->valuestring) == 0) { dup = true; break; }
                }
                if (dup) {
                    RTK_LOGW(TAG, "skill '%s': duplicate cap_group '%s'\n",
                             expected_name, g->valuestring);
                    cJSON_Delete(root);
                    out->valid = false;
                    return;
                }
                if (out->cap_group_count >= SKILL_MAX_CAP_GROUPS) {
                    RTK_LOGW(TAG, "skill '%s': too many cap_groups (max %d)\n",
                             expected_name, SKILL_MAX_CAP_GROUPS);
                    cJSON_Delete(root);
                    out->valid = false;
                    return;
                }
                strlcpy(out->cap_groups[out->cap_group_count], g->valuestring,
                        sizeof(out->cap_groups[0]));
                out->cap_group_count++;
            }
        }
    }

    cJSON_Delete(root);
    out->valid = true;
    out->has_frontmatter = true;
}

/* Load + parse a skill's frontmatter from <base>/<name>/SKILL.md.
 * Returns true and fills `out` if the SKILL.md exists and parses cleanly. */
static bool load_skill_meta(const char *base, const char *name, skill_meta_t *out)
{
    char md_path[192];
    build_md_path_in(md_path, sizeof(md_path), base, name);
    char *doc = read_file_alloc(md_path, SKILL_MD_MAX_SIZE);
    if (!doc) return false;
    parse_skill_frontmatter(doc, name, out);
    free(doc);
    return out->valid;
}

/* ---- cap_groups visibility gating (improvement #12 Inc 6, gap S1) ---- */

/* Recompute the union of cap_groups across all active skills of `session_id`,
 * then publish it as that session's LLM-visible group set. Gateable groups
 * that no active skill declares become invisible again for the session. */
static void sync_session_visible_groups(const char *session_id)
{
    const char *sid = (session_id && session_id[0]) ? session_id : DEFAULT_SESSION_ID;

    char  groups[SESSION_MAX_VISIBLE_GROUPS][CAP_GROUP_ID_MAX];
    const char *gptrs[SESSION_MAX_VISIBLE_GROUPS];
    int   gcount = 0;

    cJSON *active = load_active_skills(sid);
    const cJSON *item;
    cJSON_ArrayForEach(item, active) {
        if (!cJSON_IsString(item)) continue;
        const char *name = item->valuestring;
        const char *base = resolve_skill_base(name);
        if (!base) continue;
        skill_meta_t meta;
        if (!load_skill_meta(base, name, &meta)) continue;
        for (int i = 0; i < meta.cap_group_count && gcount < SESSION_MAX_VISIBLE_GROUPS; i++) {
            const char *gid = meta.cap_groups[i];
            /* Only gateable groups need surfacing; others are always visible. */
            if (!group_is_gateable(gid)) continue;
            bool dup = false;
            for (int k = 0; k < gcount; k++) {
                if (strcmp(groups[k], gid) == 0) { dup = true; break; }
            }
            if (dup) continue;
            strlcpy(groups[gcount], gid, sizeof(groups[0]));
            gcount++;
        }
    }
    cJSON_Delete(active);

    for (int i = 0; i < gcount; i++) gptrs[i] = groups[i];
    claw_cap_set_session_llm_visible_groups(sid, gptrs, (size_t)gcount);
}

static int skill_mgr_start(void)
{
    return RTK_SUCCESS;
}

/* ---- Skill enumeration / dedup ---- */

/* Append one "- <name>: <desc>\n" line to the *p / *rem cursor.
 *
 * The description comes from the SKILL.md frontmatter "description" field when
 * present; otherwise it falls back to the first non-heading markdown line
 * (legacy plain-markdown skills). Skills whose frontmatter fails to parse are
 * skipped (and an error has already been logged by parse_skill_frontmatter). */
static void catalog_append_line(char **p, size_t *rem, const char *base, const char *name)
{
    char md_path[192];
    build_md_path_in(md_path, sizeof(md_path), base, name);
    char *doc = read_file_alloc(md_path, SKILL_MD_MAX_SIZE);
    if (!doc) return;

    skill_meta_t meta;
    parse_skill_frontmatter(doc, name, &meta);
    if (!meta.valid) { free(doc); return; }

    char desc_buf[256];
    const char *desc = NULL;
    if (meta.has_frontmatter && meta.description[0]) {
        desc = meta.description;
    } else {
        /* Legacy: first non-heading line of the markdown body. */
        char *d = doc;
        while (*d == '#' || *d == ' ' || *d == '\n' || *d == '\r') d++;
        char *nl = strchr(d, '\n');
        if (nl) *nl = '\0';
        char *cr = strchr(d, '\r');
        if (cr) *cr = '\0';
        strlcpy(desc_buf, d, sizeof(desc_buf));
        desc = desc_buf;
    }

    int n = DiagSnPrintf(*p, *rem, "- %s: %s\n", name, desc);
    if (n > 0 && (size_t)n < *rem) { *p += n; *rem -= (size_t)n; }
    free(doc);
}

/* ---- Skill catalog context provider (merges rolfs: + vfs:) ---- */

static int collect_skill_catalog(const claw_agent_request_t *request,
                                  claw_agent_context_t *out_context,
                                  void *user_ctx)
{
    (void)request;
    (void)user_ctx;

    char *buf = malloc(4096);
    if (!buf) return RTK_FAIL;

    char *p = buf;
    size_t rem = 4096;
    int n;

    n = DiagSnPrintf(p, rem, "[Installed Skills]\n");
    if (n > 0) { p += n; rem -= (size_t)n; }

    /* 1. Built-in skills from the read-only ROLFS image (rolfs:/skills). */
    {
        void *dir = opendir(ROLFS_SKILLS_DIR);
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL && rem > 4) {
                if (ent->d_name[0] == '.') continue;
                if (!skill_name_is_safe(ent->d_name)) continue;
                catalog_append_line(&p, &rem, ROLFS_SKILLS_DIR, ent->d_name);
            }
            closedir(dir);
        }
    }

    /* 2. User skills (vfs:/skills): skip any that shadow a built-in name. */
    {
        void *dir = opendir(s_rt.skills_dir);
        if (dir) {
            struct dirent *ent;
            char rolfs_md[192];
            while ((ent = readdir(dir)) != NULL && rem > 4) {
                if (ent->d_name[0] == '.') continue;
                if (!skill_name_is_safe(ent->d_name)) continue;
                /* built-in takes precedence — skip same-named user skill */
                build_md_path_in(rolfs_md, sizeof(rolfs_md), ROLFS_SKILLS_DIR, ent->d_name);
                if (file_exists(rolfs_md)) continue;
                catalog_append_line(&p, &rem, s_rt.skills_dir, ent->d_name);
            }
            closedir(dir);
        }
    }

    n = DiagSnPrintf(p, rem,
                 "(skill_activate(name) loads a skill's doc; it lists the absolute script path(s) — run them with lua_run(path, args))\n");
    if (n > 0 && (size_t)n < rem) { p += n; rem -= (size_t)n; }

    if (p == buf + strlen("[Installed Skills]\n")) {
        free(buf);
        return RTK_FAIL;
    }

    out_context->kind    = CLAW_AGENT_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = buf;
    return RTK_SUCCESS;
}

claw_agent_context_provider_t cap_skill_catalog_provider = {
    .name     = "skill_catalog",
    .collect  = collect_skill_catalog,
    .user_ctx = NULL,
};

/* ---- execute: skill_list (merges rolfs: + vfs:, rolfs: precedence) ---- */

/* Append a directory-based skill entry to the JSON array, recording its source
 * and (when frontmatter is present) its parsed description + manage_mode.
 * Skills whose frontmatter fails to parse are skipped entirely. */
static void list_add_dir_skill(cJSON *jarr, const char *base,
                               const char *name, const char *source)
{
    skill_meta_t meta;
    if (!load_skill_meta(base, name, &meta)) return;  /* invalid frontmatter → skip */

    cJSON *entry = cJSON_CreateObject();
    if (!entry) return;
    cJSON_AddStringToObject(entry, "name", name);
    cJSON_AddStringToObject(entry, "type", "directory");
    cJSON_AddStringToObject(entry, "source", source);
    if (meta.has_frontmatter) {
        if (meta.description[0]) {
            cJSON_AddStringToObject(entry, "description", meta.description);
        }
        cJSON_AddStringToObject(entry, "manage_mode",
                                meta.manage_mode == SKILL_MODE_RUNTIME ? "runtime" : "readonly");
    }
    cJSON_AddItemToArray(jarr, entry);
}

static int cap_skill_list(const char *input_json,
                          const claw_cap_call_context_t *ctx,
                          char **output)
{
    (void)input_json;
    (void)ctx;

    cJSON *jarr = cJSON_CreateArray();
    if (!jarr) {
        claw_cap_set_output(output, "{\"skills\":[]}");
        return RTK_ERR_NOMEM;
    }

    /* 1. Built-in skills from rolfs:/skills. */
    {
        void *dir = opendir(ROLFS_SKILLS_DIR);
        if (!dir) {
            RTK_LOGW(TAG, "skill_list: opendir(%s) failed — rolfs not mounted (CONFIG_LITTLEFS_WITHIN_APP_IMG disabled?)\n", ROLFS_SKILLS_DIR);
        } else {
            struct dirent *ent;
            char md_path[192];
            while ((ent = readdir(dir)) != NULL) {
                const char *name = ent->d_name;
                if (name[0] == '.') continue;
                if (!skill_name_is_safe(name)) continue;
                build_md_path_in(md_path, sizeof(md_path), ROLFS_SKILLS_DIR, name);
                if (file_exists(md_path)) {
                    list_add_dir_skill(jarr, ROLFS_SKILLS_DIR, name, "builtin");
                }
            }
            closedir(dir);
        }
    }

    /* 2. User skills (vfs:/skills): directory format only; skip built-in shadows. */
    {
        void *dir = opendir(s_rt.skills_dir);
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                const char *name = ent->d_name;
                if (name[0] == '.') continue;
                if (!skill_name_is_safe(name)) continue;
                if (ent->d_type != DT_DIR) continue;

                char md_path[192], rolfs_md[192];
                build_md_path_in(md_path, sizeof(md_path), s_rt.skills_dir, name);
                if (!file_exists(md_path)) continue;
                /* built-in precedence: skip same-named user skill */
                build_md_path_in(rolfs_md, sizeof(rolfs_md), ROLFS_SKILLS_DIR, name);
                if (file_exists(rolfs_md)) continue;
                list_add_dir_skill(jarr, s_rt.skills_dir, name, "user");
            }
            closedir(dir);
        }
    }

    cJSON *jout = cJSON_CreateObject();
    if (!jout) {
        cJSON_Delete(jarr);
        claw_cap_set_output(output, "{\"skills\":[]}");
        return RTK_ERR_NOMEM;
    }
    cJSON_AddItemToObject(jout, "skills", jarr);
    char *s = cJSON_PrintUnformatted(jout);
    cJSON_Delete(jout);
    if (!s) { claw_cap_set_output(output, "{\"skills\":[]}"); return RTK_ERR_NOMEM; }
    *output = s;
    return RTK_SUCCESS;
}

/* ---- execute: skill_activate ---- */

static int cap_skill_activate(const char *input_json,
                              const claw_cap_call_context_t *ctx,
                              char **output)
{
    const char *session_id = (ctx && ctx->session_id) ? ctx->session_id : NULL;

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        claw_cap_set_output(output, "{\"error\":\"invalid input JSON\"}");
        return RTK_FAIL;
    }

    cJSON *jname = cJSON_GetObjectItem(root, "name");
    if (!jname || !cJSON_IsString(jname) || !jname->valuestring || !jname->valuestring[0]) {
        claw_cap_set_output(output, "{\"error\":\"missing required field: name\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    const char *name = jname->valuestring;
    if (!skill_name_is_safe(name)) {
        claw_cap_set_output(output, "{\"error\":\"invalid skill name\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    /* Built-in (rolfs:) takes precedence over a same-named user skill. */
    const char *base = resolve_skill_base(name);
    if (!base) {
        claw_cap_set_output(output,
                 "{\"error\":\"SKILL.md not found for skill '%s'. Use skill_list to see available skills.\"}", name);
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    /* Reject activation of a skill whose frontmatter is malformed. */
    {
        skill_meta_t meta;
        if (!load_skill_meta(base, name, &meta)) {
            claw_cap_set_output(output,
                     "{\"error\":\"skill '%s' has invalid SKILL.md frontmatter\"}", name);
            cJSON_Delete(root);
            return RTK_FAIL;
        }
    }

    char md_path[192];
    build_md_path_in(md_path, sizeof(md_path), base, name);
    char *md_content = read_file_alloc(md_path, SKILL_MD_MAX_SIZE);
    if (!md_content) {
        claw_cap_set_output(output,
                 "{\"error\":\"SKILL.md not readable for skill '%s'\"}", name);
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    /* Expand {CUR_SKILL_DIR} → the skill's absolute directory. */
    char *expanded = expand_cur_skill_dir(md_content, base, name);
    free(md_content);
    if (!expanded) {
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        cJSON_Delete(root);
        return RTK_ERR_NOMEM;
    }

    /* Persist this skill as active for THIS session, then re-sync the
     * session's LLM-visible cap_groups (gap S1 + S5). */
    cJSON *active = load_active_skills(session_id);
    active_skills_add(active, name);
    save_active_skills(session_id, active);
    cJSON_Delete(active);
    sync_session_visible_groups(session_id);

    /* Return SKILL.md content as tool result */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "skill", name);
    cJSON_AddStringToObject(resp, "doc", expanded);
    free(expanded);

    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (s) {
        *output = s;
    } else {
        *output = NULL;
    }
    cJSON_Delete(root);
    return s ? RTK_SUCCESS : RTK_ERR_NOMEM;
}

/* ---- execute: skill_deactivate ---- */

static int cap_skill_deactivate(const char *input_json,
                                const claw_cap_call_context_t *ctx,
                                char **output)
{
    const char *session_id = (ctx && ctx->session_id) ? ctx->session_id : NULL;
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        claw_cap_set_output(output, "{\"error\":\"invalid input JSON\"}");
        return RTK_FAIL;
    }
    cJSON *jname = cJSON_GetObjectItem(root, "name");
    if (!jname || !cJSON_IsString(jname) || !jname->valuestring || !jname->valuestring[0]) {
        claw_cap_set_output(output, "{\"error\":\"missing required field: name\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    const char *name = jname->valuestring;
    if (!skill_name_is_safe(name)) {
        claw_cap_set_output(output, "{\"error\":\"invalid skill name\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    cJSON *active = load_active_skills(session_id);
    active_skills_remove(active, name);
    save_active_skills(session_id, active);
    cJSON_Delete(active);
    sync_session_visible_groups(session_id);
    int rc = claw_cap_set_output(output, "{\"status\":\"deactivated\",\"skill\":\"%s\"}", name);
    cJSON_Delete(root);
    return rc;
}

/* ---- execute: skill_save (user skills only, writable VFS) ---- */

static int cap_skill_save(const char *input_json,
                          const claw_cap_call_context_t *ctx,
                          char **output)
{
    const char *session_id = (ctx && ctx->session_id) ? ctx->session_id : NULL;

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        claw_cap_set_output(output, "{\"error\":\"invalid input JSON\"}");
        return RTK_FAIL;
    }

    cJSON *jname = cJSON_GetObjectItem(root, "name");
    cJSON *jcode = cJSON_GetObjectItem(root, "code");
    if (!jname || !cJSON_IsString(jname) || !jname->valuestring || !jname->valuestring[0]) {
        claw_cap_set_output(output, "{\"error\":\"missing required field: name\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    if (!jcode || !cJSON_IsString(jcode) || !jcode->valuestring) {
        claw_cap_set_output(output, "{\"error\":\"missing required field: code\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    const char *name = jname->valuestring;
    if (!skill_name_is_safe(name)) {
        claw_cap_set_output(output, "{\"error\":\"invalid skill name\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    const char *code = jcode->valuestring;

    /* Built-in names are reserved (read-only ROLFS image) — refuse to shadow them. */
    {
        char rolfs_md[192];
        build_md_path_in(rolfs_md, sizeof(rolfs_md), ROLFS_SKILLS_DIR, name);
        if (file_exists(rolfs_md)) {
            claw_cap_set_output(output,
                "{\"error\":\"'%s' is a read-only built-in skill; choose a different name\"}", name);
            cJSON_Delete(root);
            return RTK_FAIL;
        }
    }

    /* doc (SKILL.md content) is now mandatory: a skill without a SKILL.md
     * has nothing for the catalog to surface and nothing for skill_activate
     * to inject — for those cases the caller should use write_file directly. */
    cJSON *jdoc = cJSON_GetObjectItem(root, "doc");
    if (!jdoc || !cJSON_IsString(jdoc) || !jdoc->valuestring[0]) {
        claw_cap_set_output(output,
            "{\"error\":\"missing required field: doc (SKILL.md content). "
            "For a script without a skill wrapper, use write_file + lua_run directly.\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    mkdir(s_rt.skills_dir, 0777);

    char dir_path[128];
    DiagSnPrintf(dir_path, sizeof(dir_path), "%s/%s", s_rt.skills_dir, name);
    mkdir(dir_path, 0777);
    char scripts_path[160];
    DiagSnPrintf(scripts_path, sizeof(scripts_path), "%s/scripts", dir_path);
    mkdir(scripts_path, 0777);

    /* Write SKILL.md */
    char md_path[160];
    DiagSnPrintf(md_path, sizeof(md_path), "%s/SKILL.md", dir_path);
    FILE *f = fopen(md_path, "w");
    if (!f) {
        claw_cap_set_output(output, "{\"error\":\"failed to write metadata: %s\"}", md_path);
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    size_t doc_len     = strlen(jdoc->valuestring);
    size_t doc_written = fwrite(jdoc->valuestring, 1, doc_len, f);
    fclose(f);
    if (doc_written != doc_len) {
        claw_cap_set_output(output, "{\"error\":\"metadata write incomplete (%u/%u bytes): %s\"}", (unsigned)doc_written, (unsigned)doc_len, md_path);
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    /* Write main.lua */
    char lua_path[160];
    DiagSnPrintf(lua_path, sizeof(lua_path), "%s/main.lua", scripts_path);
    f = fopen(lua_path, "w");
    if (!f) {
        claw_cap_set_output(output, "{\"error\":\"failed to write script: %s\"}", lua_path);
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    size_t code_len = strlen(code);
    size_t written = fwrite(code, 1, code_len, f);
    fclose(f);
    if (written != code_len) {
        claw_cap_set_output(output, "{\"error\":\"write incomplete (%u/%u bytes), disk may be full — use skill_delete to free space\"}", (unsigned)written, (unsigned)code_len);
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    int rc = claw_cap_set_output(output, "{\"status\":\"saved\",\"name\":\"%s\",\"bytes\":%u}", name, (unsigned)code_len);

    /* Auto-activate: add to persistent active_skills so the context provider injects
     * the skill's SKILL.md on the next request without requiring a separate skill_activate
     * call. Without this, skill_run would use the new code but the LLM would not have
     * the updated doc context, and repeated saves would never refresh the in-memory view. */
    if (rc == RTK_SUCCESS) {
        cJSON *active = load_active_skills(session_id);
        active_skills_add(active, name);
        save_active_skills(session_id, active);
        cJSON_Delete(active);
        sync_session_visible_groups(session_id);
    }

    cJSON_Delete(root);
    return rc;
}

/* ---- execute: skill_delete (user skills only) ---- */

static int cap_skill_delete(const char *input_json,
                            const claw_cap_call_context_t *ctx,
                            char **output)
{
    const char *session_id = (ctx && ctx->session_id) ? ctx->session_id : NULL;

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        claw_cap_set_output(output, "{\"error\":\"invalid input JSON\"}");
        return RTK_FAIL;
    }

    cJSON *jname = cJSON_GetObjectItem(root, "name");
    if (!jname || !cJSON_IsString(jname) || !jname->valuestring || !jname->valuestring[0]) {
        claw_cap_set_output(output, "{\"error\":\"missing required field: name\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    const char *name = jname->valuestring;
    if (!skill_name_is_safe(name)) {
        claw_cap_set_output(output, "{\"error\":\"invalid skill name\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    /* Built-in skills live in the read-only ROLFS image — they cannot be deleted. */
    {
        char rolfs_md[192];
        build_md_path_in(rolfs_md, sizeof(rolfs_md), ROLFS_SKILLS_DIR, name);
        if (file_exists(rolfs_md)) {
            claw_cap_set_output(output,
                "{\"error\":\"'%s' is a read-only built-in skill and cannot be deleted\"}", name);
            cJSON_Delete(root);
            return RTK_FAIL;
        }
    }

    /* Directory format: remove SKILL.md, scripts/main.lua, scripts/, dir/ */
    char dir_path[128];
    DiagSnPrintf(dir_path, sizeof(dir_path), "%s/%s", s_rt.skills_dir, name);
    char md_path[160];
    DiagSnPrintf(md_path, sizeof(md_path), "%s/SKILL.md", dir_path);
    char lua_path[160];
    DiagSnPrintf(lua_path, sizeof(lua_path), "%s/scripts/main.lua", dir_path);
    char scripts_path[160];
    DiagSnPrintf(scripts_path, sizeof(scripts_path), "%s/scripts", dir_path);

    cap_lua_file_remove(lua_path);   /* stops any running job first */
    rmdir(scripts_path);
    remove(md_path);
    int rc = rmdir(dir_path);
    if (rc != 0) {
        claw_cap_set_output(output, "{\"error\":\"skill '%s' not found\"}", name);
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    /* Also remove from this session's active_skills list + re-sync visibility */
    cJSON *active = load_active_skills(session_id);
    active_skills_remove(active, name);
    save_active_skills(session_id, active);
    cJSON_Delete(active);
    sync_session_visible_groups(session_id);

    int set_rc = claw_cap_set_output(output, "{\"status\":\"deleted\",\"name\":\"%s\"}", name);
    cJSON_Delete(root);
    return set_rc;
}

/* ---- Cap descriptors & group ---- */

static const claw_cap_descriptor_t s_desc[] = {
    {
        .id          = "skill_list",
        .name        = "skill_list",
        .family      = "skill_mgr",
        .description = "List all available Lua skills on the device.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = cap_skill_list,
    },
    {
        .id          = "skill_activate",
        .name        = "skill_activate",
        .family      = "skill_mgr",
        .description = "Activate a skill and return its SKILL.md document as operating instructions.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Skill name\"}},"
            "\"required\":[\"name\"]}",
        .execute     = cap_skill_activate,
    },
    {
        .id          = "skill_save",
        .name        = "skill_save",
        .family      = "skill_mgr",
        .description = "Save a Lua skill as <name>/SKILL.md + <name>/scripts/main.lua; doc and code are both required. For a script without a skill wrapper, use write_file + lua_run directly.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Skill id (also the directory name)\"},"
            "\"code\":{\"type\":\"string\",\"description\":\"Lua script content for scripts/main.lua\"},"
            "\"doc\":{\"type\":\"string\",\"description\":\"SKILL.md content (frontmatter + usage)\"}"
            "},"
            "\"required\":[\"name\",\"code\",\"doc\"]}",
        .execute     = cap_skill_save,
    },
    {
        .id          = "skill_delete",
        .name        = "skill_delete",
        .family      = "skill_mgr",
        .description = "Delete a user skill directory (SKILL.md + scripts/main.lua + parent dir).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Skill name\"}"
            "},"
            "\"required\":[\"name\"]}",
        .execute     = cap_skill_delete,
    },
    {
        .id          = "skill_deactivate",
        .name        = "skill_deactivate",
        .family      = "skill_mgr",
        .description = "Deactivate a skill, removing it from the persistent list so its doc is no longer injected on restart.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Skill name\"}"
            "},"
            "\"required\":[\"name\"]}",
        .execute     = cap_skill_deactivate,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "skill_mgr",
    .plugin_name      = "cap_skill_mgr",
    .version          = "2",
    .descriptors      = s_desc,
    .descriptor_count = 5,
    .group_start      = skill_mgr_start,
};

/* ---- Context provider: inject active skills' SKILL.md into system prompt ---- */

static int collect_skill_context(const claw_agent_request_t *request,
                                 claw_agent_context_t *out_context,
                                 void *user_ctx)
{
    (void)user_ctx;
    const char *session_id = (request && request->session_id) ? request->session_id : NULL;

    cJSON *active = load_active_skills(session_id);
    int count = cJSON_GetArraySize(active);
    if (count == 0) { cJSON_Delete(active); return RTK_FAIL; }

    /* Collect all SKILL.md contents */
    size_t total = 0;
    char *docs[MAX_ACTIVE_SKILLS]  = {0};
    char *names[MAX_ACTIVE_SKILLS] = {0};   /* heap-allocated copies, valid after cJSON_Delete */
    int valid = 0;

    int needs_save = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, active) {
        if (!cJSON_IsString(item)) continue;
        const char *name = item->valuestring;
        const char *base = resolve_skill_base(name);
        char *doc = NULL;
        if (base) {
            char md_path[192];
            build_md_path_in(md_path, sizeof(md_path), base, name);
            char *raw = read_file_alloc(md_path, SKILL_MD_MAX_SIZE);
            if (raw) {
                doc = expand_cur_skill_dir(raw, base, name);
                free(raw);
            }
        }
        if (doc) {
            docs[valid]  = doc;
            names[valid] = strdup(name);   /* copy before cJSON_Delete frees valuestring */
            total += strlen("--- Skill: ") + strlen(name) + strlen(" ---\n") + strlen(doc) + 2;
            valid++;
        } else {
            needs_save = 1; /* stale entry — will be cleaned after iteration */
        }
        if (valid >= MAX_ACTIVE_SKILLS) break;
    }

    /* Clean up stale entries (skills deleted from VFS while session was active).
     * Must sync gating even if already restored — skill set changed. */
    if (needs_save) {
        cJSON *clean = cJSON_CreateArray();
        if (clean) {
            for (int i = 0; i < valid; i++)
                cJSON_AddItemToArray(clean, cJSON_CreateString(names[i]));
            save_active_skills(session_id, clean);
            cJSON_Delete(clean);
        }
        sync_session_visible_groups(session_id);
        /* Re-mark as restored with updated state */
        gating_mark_restored(session_id);
    }
    cJSON_Delete(active);

    /* Restore session gating on first request after boot.
     * apply_base_visibility() at boot sets global base visibility only;
     * per-session skill-gated groups must be re-applied once per boot.
     * After that, gating is stable — skip the redundant file read + sync
     * on every subsequent request for this session. */
    if (!gating_is_restored(session_id)) {
        sync_session_visible_groups(session_id);
        gating_mark_restored(session_id);
    }

    if (valid == 0) return RTK_FAIL;

    char *content = malloc(total + 1);
    if (!content) {
        for (int i = 0; i < valid; i++) { free(docs[i]); free(names[i]); }
        return RTK_FAIL;
    }

    char *p = content;
    char *end = content + total + 1;
    for (int i = 0; i < valid; i++) {
        int written = DiagSnPrintf(p, (size_t)(end - p), "--- Skill: %s ---\n%s\n",
                               names[i], docs[i]);
        if (written > 0 && written < (int)(end - p)) {
            p += written;
        } else if (written > 0) {
            RTK_LOGW(TAG, "skill context buffer truncated at '%s'\n", names[i]);
        }
        free(docs[i]);
        free(names[i]);
    }

    out_context->kind    = CLAW_AGENT_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = content;
    return RTK_SUCCESS;
}

claw_agent_context_provider_t cap_skill_mgr_context_provider = {
    .name       = "active_skills",
    .collect    = collect_skill_context,
    .user_ctx   = NULL,
    .quiet_skip = true,  /* skips when no skills are active — expected */
};



/* ---- Public init ---- */

int cap_skill_mgr_init(const cap_skill_mgr_config_t *config)
{
    if (!config || !config->skills_dir) {
        return RTK_ERR_BADARG;
    }

    strncpy(s_rt.skills_dir, config->skills_dir, sizeof(s_rt.skills_dir) - 1);
    s_rt.skills_dir[sizeof(s_rt.skills_dir) - 1] = '\0';

    mkdir(s_rt.skills_dir, 0777);

    int err = claw_cap_register_group(&s_group);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "Failed to register group: %d\n", (int)err);
        return err;
    }

    RTK_LOGI(TAG, "Initialized (user skills_dir=%s, builtins=%s)\n",
             s_rt.skills_dir, ROLFS_SKILLS_DIR);
    return RTK_SUCCESS;
}

void cap_skill_mgr_apply_base_visibility(void)
{
    /* PRECONDITION: must be called after claw_config_init(). If config is not
     * yet initialised, hidden_count is 0 and all groups will be visible — which
     * is incorrect.  The call order in ameba_claw_main.c guarantees this, but
     * refactors that move cap_skill_mgr_init() earlier must preserve it. */

    /* Build the global visible list = all registered groups − gateable − config-hidden. */
    claw_cap_group_list_t groups = claw_cap_list_groups();
    const claw_cap_visibility_config_t *vis = &claw_config_get()->cap_visibility;

    /* claw_cap caps the global list at CLAW_CAP_HIDDEN_MAX; keep within that. */
    static char        base[CLAW_CAP_HIDDEN_MAX][CAP_GROUP_ID_MAX];
    const char        *bptrs[CLAW_CAP_HIDDEN_MAX];
    int                bcount = 0;
    int                skipped = 0;

    int hidden_by_config = 0;

    for (size_t i = 0; i < groups.count && bcount < CLAW_CAP_HIDDEN_MAX; i++) {
        const char *gid = groups.items[i].group_id;
        if (!gid || !gid[0]) continue;
        if (group_is_gateable(gid)) { skipped++; continue; }
        if (group_is_hidden(gid, vis)) { skipped++; hidden_by_config++; continue; }
        strlcpy(base[bcount], gid, sizeof(base[0]));
        bptrs[bcount] = base[bcount];
        bcount++;
    }

    if (bcount == 0 && hidden_by_config > 0) {
        /* User explicitly hid every visible group — use the dedicated hide-all
         * API so the cap layer does not misinterpret count=0 as "show all". */
        claw_cap_hide_all_groups();
        RTK_LOGI(TAG, "base visibility: 0 group(s) visible (all hidden), %d gated/hidden (rc=0)\n",
                 skipped);
        return;
    }

    int rc = claw_cap_set_llm_visible_groups(bptrs, (size_t)bcount);
    RTK_LOGI(TAG, "base visibility: %d group(s) visible, %d gated/hidden (rc=%d)\n",
             bcount, skipped, rc);
}
#undef group_is_hidden

/* ---- Lifecycle registration (claw_cap_registry): INIT + AGENT ----
 * on_agent applies the base LLM-visibility snapshot (depends only on all groups
 * being registered — i.e. after claw_cap_start_all, which registry_run(AGENT)
 * always follows — not on the agent) then adds the two skill context providers,
 * preserving the historical provider order (context then catalog). */
static void skill_mgr_on_init(const claw_config_t *cfg)
{
    (void)cfg;
    const cap_skill_mgr_config_t c = { .skills_dir = "vfs:/skills" };
    cap_skill_mgr_init(&c);
}

static void skill_mgr_on_agent(const claw_config_t *cfg)
{
    (void)cfg;
    cap_skill_mgr_apply_base_visibility();
    claw_agent_add_context_provider(&cap_skill_mgr_context_provider);
    claw_agent_add_context_provider(&cap_skill_catalog_provider);
}

CLAW_CAP_REGISTER(skill_mgr, {
    .group    = "skill_mgr",
    .order    = 20,
    .on_init  = skill_mgr_on_init,
    .on_agent = skill_mgr_on_agent,
});
