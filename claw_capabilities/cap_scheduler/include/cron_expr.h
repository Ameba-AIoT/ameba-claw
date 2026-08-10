/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * cron_expr — a small, self-contained 5-field cron parser + next-fire scanner.
 *
 * Kept as its own translation unit (no scheduler / RTOS deps beyond libc time)
 * so the parse + scan logic can be unit-tested in isolation — the range / list /
 * step / month-end edge cases are where cron implementations usually break.
 *
 * Field order: "minute hour day-of-month month day-of-week".
 * Each field supports:  *   *\/N   a   a-b   a,b,c   a-b/N   a/N
 * plus 3-letter month names (jan..dec) and weekday names (sun..sat), and the
 * weekday alias 7 == 0 (Sunday). Day-of-month vs day-of-week follow the classic
 * Vixie-cron rule: if BOTH are restricted the match is OR, otherwise AND.
 */
#pragma once
#include <stdbool.h>
#include <time.h>

typedef struct {
    bool minute[60];   /* 0..59 */
    bool hour[24];     /* 0..23 */
    bool mday[32];     /* 1..31 (index 0 unused) */
    bool month[13];    /* 1..12 (index 0 unused) */
    bool wday[7];      /* 0..6, 0 = Sunday */
    bool mday_restricted;  /* false if day-of-month field was "*" */
    bool wday_restricted;  /* false if day-of-week  field was "*" */
    bool valid;
} cron_expr_t;

/* Parse a 5-field cron string. Returns true on success (out->valid = true),
 * false on any syntax error (out is left with valid = false). */
bool cron_parse(const char *expr, cron_expr_t *out);

/* Find the next LOCAL epoch (seconds in the local frame, i.e. utc + offset)
 * that matches `c`, strictly after `after_local`. The caller works in the local
 * frame: pass now_utc + offset_sec, and subtract offset_sec from the result to
 * get UTC. Returns (time_t)-1 if no match within CLAW_SCHEDULER_CRON_SCAN_MAX
 * minutes (e.g. an impossible expression like "0 0 30 2 *"). */
time_t cron_next_after_local(const cron_expr_t *c, time_t after_local);
