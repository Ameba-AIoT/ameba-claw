/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * cap_honesty.c — post-turn consistency audit observer.
 *
 * Each completed agent round is checked against a small rule table:
 *   needs_tool      — the tool the model would have to call to genuinely act
 *   claim_kw[]      — phrases the model uses when it asserts the action happened
 *
 * If final_text contains any claim keyword but tool_calls_csv lacks the
 * matching tool name, we emit a WARN. We never block the response — this is
 * an observability hook, not an enforcement layer.
 *
 * Two precision tweaks vs the naive substring approach:
 *  - Tool match is token-bounded by wrapping tool_calls_csv with commas
 *    and searching for ",<tool>,". Otherwise "lua_run" would be reported
 *    as missing whenever the model called a derivative like "lua_run_async",
 *    flipping a true positive into a false negative.
 *  - Claim keyword search is limited to the first ~200 bytes of final_text.
 *    Genuine action claims sit at the start of the model's reply; user-echo
 *    recaps that mention the same keyword tend to live further down. This
 *    trades a few deep-buried false negatives for far fewer false positives.
 */

#include "cap_honesty.h"
#include "claw_compat.h"
#include "claw_cap_registry.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define TAG "honesty"

#define HONESTY_HEAD_SCAN_BYTES 200
#define HONESTY_TOOLS_BUF       256
#define HONESTY_NEEDLE_BUF       64

void cap_honesty_observe_completion(const claw_agent_completion_summary_t *s,
                                    void *user_ctx)
{
    (void)user_ctx;
    if (!s || !s->final_text || !s->final_text[0]) return;

    static const struct {
        const char *needs_tool;
        const char *claim_kw[6];
    } rules[] = {
        { "lua_run",        { "已运行", "已执行", "executed", "ran",       NULL } },
        { "skill_activate", { "已激活", "activated",                       NULL } },
        { "memory_forget",  { "已忘记", "已删除", "forgotten", "forgot",   NULL } },
    };

    /* Wrap tool_calls_csv with comma boundaries so we can match exact tool
     * names with strstr without colliding on derivative names. Falls back
     * to a plain substring search only if the csv would not fit in the
     * static buffer. */
    const char *raw_tools = s->tool_calls_csv ? s->tool_calls_csv : "";
    char wrapped_tools[HONESTY_TOOLS_BUF];
    bool wrap_ok = false;
    if (strlen(raw_tools) + 3 < sizeof(wrapped_tools)) {
        snprintf(wrapped_tools, sizeof(wrapped_tools), ",%s,", raw_tools);
        wrap_ok = true;
    }

    /* Bound claim-keyword search to first N bytes (UTF-8: a Chinese keyword
     * may span the boundary, but the worst case is a missed match — we
     * never produce a false positive from truncation). */
    char head[HONESTY_HEAD_SCAN_BYTES + 1];
    size_t hlen = strlen(s->final_text);
    if (hlen > HONESTY_HEAD_SCAN_BYTES) hlen = HONESTY_HEAD_SCAN_BYTES;
    memcpy(head, s->final_text, hlen);
    head[hlen] = '\0';

    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        bool tool_invoked;
        if (wrap_ok) {
            char needle[HONESTY_NEEDLE_BUF];
            snprintf(needle, sizeof(needle), ",%s,", rules[i].needs_tool);
            tool_invoked = (strstr(wrapped_tools, needle) != NULL);
        } else {
            /* Pathological-length csv: keep legacy substring behaviour
             * rather than going silent. */
            tool_invoked = (strstr(raw_tools, rules[i].needs_tool) != NULL);
        }
        if (tool_invoked) continue;

        for (size_t j = 0; rules[i].claim_kw[j]; j++) {
            if (strstr(head, rules[i].claim_kw[j])) {
                RTK_LOGW(TAG,
                    "req=%u claims '%s' but no '%s' call (text=%.80s)\n",
                    (unsigned)s->request_id, rules[i].claim_kw[j],
                    rules[i].needs_tool, s->final_text);
                return;
            }
        }
    }
}

/* ---- Lifecycle registration (claw_cap_registry): AGENT phase only ----
 * cap_honesty registers no cap group and has no init; it only installs a
 * completion observer. The `group` id here is purely a label for the future
 * runtime enable-list. Observer ordering vs. the core memory_extract observer
 * is not significant (both are independent fire-and-forget completion hooks). */
static void honesty_on_agent(const claw_config_t *cfg)
{
    (void)cfg;
    claw_agent_add_completion_observer(cap_honesty_observe_completion, NULL);
}
CLAW_CAP_REGISTER(honesty, {
    .group    = "honesty",
    .order    = 35,
    .on_agent = honesty_on_agent,
});
