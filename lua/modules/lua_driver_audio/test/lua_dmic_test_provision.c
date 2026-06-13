/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_dmic_test_provision.c — Writes the DMIC SNR/THD test script to VFS on boot.
**
** DMIC wiring (RTL8721F):
**   PB3 (0x23) = DMIC_CLK
**   PB4 (0x24) = DMIC_DATA
** Pin numbers are defined in the Lua test script (not in driver code).
**
** Speaker wiring (RTL8721F → MAX98357A):
**   PA25 (0x19) = I2S BCLK
**   PA26 (0x1A) = I2S WS (LRCLK)
**   PA29 (0x1D) = I2S DIO3 (DATA OUT)
**
** Test flow (vfs:test_dmic.lua):
**   1. Start 1 kHz sine on speaker at SPEAKER_VOL via audio.new_output + play_tone.
**      Speaker (TX) MUST open BEFORE DMIC (RX) so audio_hw_tx_open sets
**      s_sport0_tx_active; DMIC open then skips AUDIO_SP_Reset.
**      Both use 48 kHz to share the SPORT0 BCLK.
**   2. Open DMIC stereo 48 kHz 16-bit (ADC gain 0x3F), passing PB3/PB4 pin numbers.
**   3. Run 5 iterations of audio.snr_thd(), printing SNR and THD for each channel.
**   4. Report OVERALL PASS if at least 3 iterations pass the 20 dB threshold.
**
** Trigger via:
**   AT+CLAW=dmic[,<vol>]        — run via dedicated Lua task (blocking until done)
**                                 vol defaults to 0.2; range 0.0–1.0
**   dofile("vfs:test_dmic.lua") — run from Lua REPL (uses default SPEAKER_VOL=0.2)
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* DMIC SNR/THD test script.
 * SPEAKER_VOL and DMIC_EXTERNAL are injected at runtime by lua_dmic_run(). */
static const char s_dmic_script[] =
    "-- DMIC SNR/THD test\n"
    "-- DMIC wiring: PB3=CLK (0x23), PB4=DATA (0x24)\n"
    "-- Speaker wiring: PA25(0x19)=BCLK  PA26(0x1A)=WS  PA29(0x1D)=DOUT\n"
    "-- DMIC_EXTERNAL=true  : no speaker; user plays 1kHz from phone\n"
    "-- DMIC_EXTERNAL=false : use onboard speaker (SPEAKER_VOL sets volume)\n"
    "if not DMIC_EXTERNAL then DMIC_EXTERNAL = false end\n"
    "if not SPEAKER_VOL   then SPEAKER_VOL   = '0.2'  end\n"
    "local audio = require('audio')\n"
    "local sys   = require('sys')\n"
    "\n"
    "local DMIC_CLK_PIN  = 0x23   -- PB3\n"
    "local DMIC_DATA_PIN = 0x24   -- PB4\n"
    "local BCLK_PIN      = 0x19   -- PA25\n"
    "local WS_PIN        = 0x1A   -- PA26\n"
    "local DOUT_PIN      = 0x1D   -- PA29\n"
    "\n"
    "if DMIC_EXTERNAL then\n"
    "    print('[dmic_snr] DMIC SNR/THD test start (ext source)')\n"
    "else\n"
    "    print('[dmic_snr] DMIC SNR/THD test start (vol='..SPEAKER_VOL..')')\n"
    "end\n"
    "\n"
    "-- Always use 16 kHz: 1kHz falls on exact bin 32 in the 512-pt FFT (no spectral\n"
    "-- leakage), and 16kHz I2S is valid for both TX (speaker) and RX (DMIC) on SPORT0.\n"
    "local dmic_sr  = 16000\n"
    "local adc_gain = DMIC_EXTERNAL and 0x7F or 0x3F\n"
    "\n"
    "-- IMPORTANT: Open speaker (TX) BEFORE DMIC (RX).\n"
    "-- audio_hw_tx_open sets s_sport0_tx_active; DMIC open then skips AUDIO_SP_Reset\n"
    "-- so the already-running TX config is preserved when RX starts.\n"
    "local hs = nil\n"
    "if DMIC_EXTERNAL then\n"
    "    print('[dmic_snr] READY: play 1kHz from external source now, measuring in 10s...')\n"
    "else\n"
    "    local vol = tonumber(SPEAKER_VOL) or 0.2\n"
    "    hs = audio.new_output(16000, 2, 16, BCLK_PIN, WS_PIN, DOUT_PIN)\n"
    "    if hs == nil then\n"
    "        print('[dmic_snr] FAIL: new_output')\n"
    "        return\n"
    "    end\n"
    "    audio.play_tone(hs, 1000, vol)\n"
    "    print('[dmic_snr] 1 kHz sine started, waiting 500ms for AudioTrack to init...')\n"
    "    sys.sleep_ms(500)\n"
    "end\n"
    "\n"
    "local h, err = audio.new_input(dmic_sr, 2, 16, adc_gain, DMIC_CLK_PIN, DMIC_DATA_PIN)\n"
    "if h == nil then\n"
    "    print('[dmic_snr] FAIL open: ' .. tostring(err))\n"
    "    if hs ~= nil then audio.close(hs) end\n"
    "    return\n"
    "end\n"
    "print('[dmic_snr] DMIC open OK (' .. dmic_sr .. ' Hz)')\n"
    "\n"
    "if DMIC_EXTERNAL then\n"
    "    sys.sleep_ms(10000)\n"
    "else\n"
    "    print('[dmic_snr] READY: waiting 1.5s to stabilize...')\n"
    "    sys.sleep_ms(1500)\n"
    "end\n"
    "\n"
    "local pass_count = 0\n"
    "local iterations = 5\n"
    "for i = 1, iterations do\n"
    "    local s1, t1, s2, t2, ok, pf = audio.snr_thd(h)\n"
    "    print(string.format(\n"
    "        '[dmic_snr] #%d ch1_snr=%.1fdB ch1_thd=%.1fdB'\n"
    "        ..' ch2_snr=%.1fdB ch2_thd=%.1fdB  peak=%.0fHz  %s',\n"
    "        i, s1, t1, s2, t2, pf or 0, ok and 'PASS' or 'FAIL'))\n"
    "    if ok then pass_count = pass_count + 1 end\n"
    "    sys.sleep_ms(200)\n"
    "end\n"
    "\n"
    "audio.close(h)\n"
    "if hs ~= nil then audio.close(hs) end\n"
    "\n"
    "if pass_count >= 3 then\n"
    "    print(string.format('[dmic_snr] OVERALL PASS (%d/%d)', pass_count, iterations))\n"
    "else\n"
    "    print(string.format('[dmic_snr] OVERALL FAIL (%d/%d)', pass_count, iterations))\n"
    "end\n";

void lua_driver_audio_dmic_provision(void)
{
    const char *path = "vfs:test_dmic.lua";
    { FILE *_ck = fopen(path, "r"); if (_ck) { fclose(_ck); return; } }

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fwrite(s_dmic_script, 1, strlen(s_dmic_script), f);
    fclose(f);
}

/* ---- On-demand execution via AT+CLAW=dmic[,<vol>] ---- */

typedef struct {
    char             *script;  /* heap-allocated; freed after task completes */
    SemaphoreHandle_t done;
} dmic_task_arg_t;

static void dmic_lua_task(void *param)
{
    dmic_task_arg_t *arg = (dmic_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[dmic] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[dmic] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[dmic] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    free(arg->script);
    SemaphoreHandle_t done = arg->done;
    free(arg);
    xSemaphoreGive(done);
    rtos_task_delete(NULL);
}

/* vol: speaker volume string (e.g. "0.4"), or "ext" for external source mode.
 * NULL or "" uses default "0.2". */
void lua_dmic_run(const char *vol)
{
    if (!vol || vol[0] == '\0') {
        vol = "0.2";
    }

    char preamble[128];
    preamble[0] = '\0';
    if (strcmp(vol, "ext") == 0) {
        strlcat(preamble, "DMIC_EXTERNAL=true\n", sizeof(preamble));
    } else {
        strlcat(preamble, "DMIC_EXTERNAL=false\nSPEAKER_VOL='", sizeof(preamble));
        strlcat(preamble, vol, sizeof(preamble));
        strlcat(preamble, "'\n", sizeof(preamble));
    }

    size_t total = strlen(preamble) + strlen(s_dmic_script) + 1;
    char *script = malloc(total);
    if (!script) {
        printf("[dmic] malloc failed\n");
        return;
    }
    strcpy(script, preamble);
    strcat(script, s_dmic_script);

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[dmic] semaphore create failed\n");
        free(script);
        return;
    }

    dmic_task_arg_t *arg = (dmic_task_arg_t *)malloc(sizeof(dmic_task_arg_t));
    if (!arg) {
        printf("[dmic] malloc failed\n");
        free(script);
        vSemaphoreDelete(done);
        return;
    }
    arg->script = script;
    arg->done   = done;

    if (rtos_task_create(NULL, "dmic_lua", dmic_lua_task, arg,
                         32768, 1) != RTK_SUCCESS) {
        printf("[dmic] task create failed\n");
        free(script);
        free(arg);
        vSemaphoreDelete(done);
        return;
    }

    /* Wait up to 60 s: ext=10 s wait + test; speaker=2 s stabilize + test */
    xSemaphoreTake(done, pdMS_TO_TICKS(60000));
    vSemaphoreDelete(done);
}

/* =========================================================
 * AT+CLAW=rec[,<path>[,<ms>]]   — record DMIC to WAV file
 * AT+CLAW=play[,<path>]         — play WAV file via speaker
 * ========================================================= */

typedef struct {
    char             *script;
    SemaphoreHandle_t done;
    int               timeout_ms;
} audio_task_arg_t;

static void audio_lua_task(void *param)
{
    audio_task_arg_t *arg = (audio_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[audio_lua] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[audio_lua] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[audio_lua] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    free(arg->script);
    SemaphoreHandle_t done = arg->done;
    int timeout_ms = arg->timeout_ms;
    free(arg);
    xSemaphoreGive(done);
    (void)timeout_ms;
    rtos_task_delete(NULL);
}

static void audio_run_script(const char *script, int timeout_ms)
{
    char *s = strdup(script);
    if (!s) { printf("[audio_lua] oom\n"); return; }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) { free(s); return; }

    audio_task_arg_t *arg = (audio_task_arg_t *)malloc(sizeof(audio_task_arg_t));
    if (!arg) { free(s); vSemaphoreDelete(done); return; }
    arg->script     = s;
    arg->done       = done;
    arg->timeout_ms = timeout_ms;

    if (rtos_task_create(NULL, "audio_lua", audio_lua_task, arg,
                         32768, 1) != RTK_SUCCESS) {
        free(s); free(arg); vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, pdMS_TO_TICKS(timeout_ms));
    vSemaphoreDelete(done);
}

/* DMIC pins: PB3=CLK(0x23), PB4=DATA(0x24)
 * Speaker pins: PA25(0x19)=BCLK  PA26(0x1A)=WS  PA29(0x1D)=DOUT */
void lua_audio_rec_run(const char *path, int duration_ms)
{
    char script[512];
    DiagSnPrintf(script, sizeof(script),
        "local a=require('audio')\n"
        "local h=a.new_input(16000,1,16,0x3F,0x23,0x24)\n"
        "if not h then print('[rec] DMIC open failed') return end\n"
        "print('[rec] recording %d ms -> %s')\n"
        "local ok,err=a.record_wav(h,'%s',%d)\n"
        "a.close(h)\n"
        "if ok then print('[rec] done') else print('[rec] FAIL: '..tostring(err)) end\n",
        duration_ms, path, path, duration_ms);

    /* timeout = duration + 5 s margin */
    audio_run_script(script, duration_ms + 5000);
}

void lua_audio_play_run(const char *path)
{
    char script[512];
    DiagSnPrintf(script, sizeof(script),
        "local a=require('audio')\n"
        "local h=a.new_output(16000,1,16,0x19,0x1A,0x1D)\n"
        "if not h then print('[play] speaker open failed') return end\n"
        "print('[play] playing %s')\n"
        "local ok,err=a.play_wav(h,'%s')\n"
        "a.close(h)\n"
        "if ok then print('[play] done') else print('[play] FAIL: '..tostring(err)) end\n",
        path, path);

    /* 120 s maximum for any file */
    audio_run_script(script, 120000);
}
