/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cron_expr.h"
#include "ameba_claw_defs.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Map a 3-letter month / weekday name to its number, or -1 if not a name. */
static int name_to_num(const char *s, int is_month, int is_wday)
{
    static const char *mon[] = {"jan","feb","mar","apr","may","jun",
                                "jul","aug","sep","oct","nov","dec"};
    static const char *wd[]  = {"sun","mon","tue","wed","thu","fri","sat"};
    char low[4];
    int i;
    for (i = 0; i < 3 && s[i]; i++) low[i] = (char)tolower((unsigned char)s[i]);
    low[i] = '\0';
    if (i < 3) return -1;
    if (is_month) for (int k = 0; k < 12; k++) if (strcmp(low, mon[k]) == 0) return k + 1;
    if (is_wday)  for (int k = 0; k < 7;  k++) if (strcmp(low, wd[k])  == 0) return k;
    return -1;
}

/* Parse a single value token (number or name) → *out. Returns false on error. */
static bool parse_val(const char *tok, int is_month, int is_wday, int *out)
{
    if (!tok[0]) return false;
    if (isalpha((unsigned char)tok[0])) {
        int v = name_to_num(tok, is_month, is_wday);
        if (v < 0) return false;
        *out = v;
        return true;
    }
    char *end;
    long v = strtol(tok, &end, 10);
    if (*end != '\0') return false;
    *out = (int)v;
    return true;
}

static bool set_range(bool *set, int lo, int hi, int a, int b, int step)
{
    if (step <= 0) return false;
    if (a < lo || b > hi || a > b) return false;
    for (int v = a; v <= b; v += step) set[v] = true;
    return true;
}

/* Parse one whitespace-separated field into the membership set [lo..hi]. */
static bool parse_field(const char *field, bool *set, int lo, int hi,
                        int is_month, int is_wday)
{
    for (int i = lo; i <= hi; i++) set[i] = false;

    const char *p = field;
    bool any = false;

    while (*p) {
        /* Copy one comma-separated token. */
        char tok[24];
        size_t n = 0;
        while (*p && *p != ',' && n < sizeof(tok) - 1) tok[n++] = *p++;
        tok[n] = '\0';
        while (*p && *p != ',') p++;   /* skip an over-long token remainder */
        if (*p == ',') p++;
        if (n == 0) return false;

        /* Optional step: "<range>/N". */
        int step = 1;
        char *slash = strchr(tok, '/');
        bool has_slash = (slash != NULL);
        if (has_slash) {
            *slash = '\0';
            char *e;
            long s = strtol(slash + 1, &e, 10);
            if (*e != '\0' || s <= 0) return false;
            step = (int)s;
        }

        int a, b;
        if (strcmp(tok, "*") == 0) {
            a = lo; b = hi;
        } else {
            char *dash = strchr(tok, '-');
            if (dash) {
                *dash = '\0';
                if (!parse_val(tok, is_month, is_wday, &a)) return false;
                if (!parse_val(dash + 1, is_month, is_wday, &b)) return false;
            } else {
                if (!parse_val(tok, is_month, is_wday, &a)) return false;
                b = has_slash ? hi : a;   /* "a/N" → a..hi step N; plain "a" → a */
            }
        }

        /* Weekday 7 is an alias for 0 (Sunday); normalise before range fill. */
        if (is_wday) {
            if (a == 7) a = 0;
            if (b == 7) b = 0;
            if (a > b) { int t = a; a = b; b = t; }
        }

        if (!set_range(set, lo, hi, a, b, step)) return false;
        any = true;
    }
    return any;
}

bool cron_parse(const char *expr, cron_expr_t *out)
{
    if (!expr || !out) return false;
    memset(out, 0, sizeof(*out));

    /* Split into exactly 5 whitespace-separated fields. */
    char fields[5][40];
    int fi = 0;
    const char *p = expr;
    while (*p && fi < 5) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        size_t n = 0;
        while (*p && *p != ' ' && *p != '\t' && n < sizeof(fields[0]) - 1)
            fields[fi][n++] = *p++;
        fields[fi][n] = '\0';
        while (*p && *p != ' ' && *p != '\t') p++;   /* skip over-long token */
        fi++;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (fi != 5 || *p != '\0') return false;   /* wrong field count / trailing junk */

    if (!parse_field(fields[0], out->minute, 0, 59, 0, 0)) return false;
    if (!parse_field(fields[1], out->hour,   0, 23, 0, 0)) return false;
    if (!parse_field(fields[2], out->mday,   1, 31, 0, 0)) return false;
    if (!parse_field(fields[3], out->month,  1, 12, 1, 0)) return false;
    if (!parse_field(fields[4], out->wday,   0, 6,  0, 1)) return false;

    out->mday_restricted = false;
    for (int i = 1; i <= 31; i++) if (!out->mday[i]) { out->mday_restricted = true; break; }
    out->wday_restricted = false;
    for (int i = 0; i <= 6;  i++) if (!out->wday[i]) { out->wday_restricted = true; break; }

    out->valid = true;
    return true;
}

time_t cron_next_after_local(const cron_expr_t *c, time_t after_local)
{
    if (!c || !c->valid) return (time_t)-1;

    /* Start at the next whole minute strictly after the anchor. */
    time_t start = ((after_local / 60) + 1) * 60;

    for (long i = 0; i < CLAW_SCHEDULER_CRON_SCAN_MAX; i++) {
        time_t t = start + (time_t)i * 60;
        struct tm tm;
        gmtime_r(&t, &tm);   /* local frame: t is already utc+offset */

        if (!c->minute[tm.tm_min])      continue;
        if (!c->hour[tm.tm_hour])       continue;
        if (!c->month[tm.tm_mon + 1])   continue;

        bool dom = c->mday[tm.tm_mday];
        bool dow = c->wday[tm.tm_wday];
        /* Vixie rule: both restricted → OR, else AND. */
        bool day_ok = (c->mday_restricted && c->wday_restricted)
                      ? (dom || dow)
                      : (dom && dow);
        if (!day_ok) continue;

        return t;
    }
    return (time_t)-1;   /* no match within scan window (e.g. Feb 30) */
}
