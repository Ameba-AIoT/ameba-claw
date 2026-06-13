/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_speaker_test_provision.c — Writes the speaker output test script to VFS on boot.
**
** Speaker wiring (RTL8721F → MAX98357A):
**   PA25 (0x19) = I2S BCLK
**   PA26 (0x1A) = I2S WS (LRCLK)
**   PA29 (0x1D) = I2S DIO3 (DATA OUT, via PAD_BIT_SP0_DIO3_MUXSEL=1 → DOUT_0)
** Pin configuration is passed to audio.new_output(); not hardcoded in driver code.
**
** Test flow controlled by SPEAKER_MODE (injected at runtime):
**   "sine"  — Step 2 only: 1 kHz sine wave 20 s
**   "nokia" — Step 3 only: nokia.wav from VFS
**   "all"   — Steps 1-3 (default)
**
** Trigger via:
**   AT+CLAW=speaker[,sine|nokia]   — run via dedicated Lua task
**   dofile("vfs:test_speaker.lua") — run from Lua REPL (SPEAKER_MODE defaults to "all")
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "nokia_wav.h"

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* Speaker test script using native audio.new_output API.
 * SPEAKER_MODE and SPEAKER_VOL are injected at runtime by lua_speaker_run(). */
static const char s_speaker_script[] =
    "-- Speaker output test: MAX98357A via I2S\n"
    "-- PA25(0x19)=BCLK  PA26(0x1A)=WS  PA29(0x1D)=DIO3/DOUT_0\n"
    "if not SPEAKER_MODE then SPEAKER_MODE = 'all' end\n"
    "if not SPEAKER_VOL   then SPEAKER_VOL = '0.2' end\n"
    "local audio = require('audio')\n"
    "local sys   = require('sys')\n"
    "local vol = tonumber(SPEAKER_VOL) or 0.2\n"
    "local BCLK_PIN = 0x19   -- PA25\n"
    "local WS_PIN   = 0x1A   -- PA26\n"
    "local DOUT_PIN = 0x1D   -- PA29\n"
    "\n"
    "print('[speaker] Speaker test start (mode='..SPEAKER_MODE..', vol='..SPEAKER_VOL..')')\n"
    "\n"
    "if SPEAKER_MODE == 'all' then\n"
    "    print('[speaker] Step 1: audio module loaded OK')\n"
    "end\n"
    "\n"
    "if SPEAKER_MODE == 'all' or SPEAKER_MODE == 'sine' then\n"
    "    print('[speaker] Step 2: 1 kHz sine wave, 20 s')\n"
    "    local h = audio.new_output(16000, 2, 16, BCLK_PIN, WS_PIN, DOUT_PIN)\n"
    "    if h == nil then\n"
    "        print('[speaker] FAIL: new_output')\n"
    "        return\n"
    "    end\n"
    "    audio.play_tone(h, 1000, vol)\n"
    "    sys.sleep_ms(20000)\n"
    "    audio.close(h)\n"
    "end\n"
    "\n"
    "if SPEAKER_MODE == 'all' or SPEAKER_MODE == 'nokia' then\n"
    "    print('[speaker] Step 3: nokia.wav')\n"
    "    local h = audio.new_output(8000, 1, 16, BCLK_PIN, WS_PIN, DOUT_PIN)\n"
    "    if h == nil then\n"
    "        print('[speaker] FAIL: new_output for nokia.wav')\n"
    "        return\n"
    "    end\n"
    "    local ok, err = audio.play_wav(h, 'vfs:nokia.wav', vol)\n"
    "    if not ok then\n"
    "        print('[speaker] FAIL: play_wav: ' .. tostring(err))\n"
    "    end\n"
    "    audio.close(h)\n"
    "end\n"
    "\n"
    "print('[speaker] Speaker test done')\n";

void lua_driver_audio_speaker_provision(void)
{
    const char *path = "vfs:test_speaker.lua";
    { FILE *_ck = fopen(path, "r"); if (_ck) { fclose(_ck); return; } }

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fwrite(s_speaker_script, 1, strlen(s_speaker_script), f);
    fclose(f);

    /* Provision nokia.wav so "audio.play_wav(h, 'vfs:nokia.wav')" works out of the box. */
    const char *wav_path = "vfs:nokia.wav";
    FILE *wf = fopen(wav_path, "wb");
    if (wf == NULL) {
        return;
    }
    fwrite(s_nokia_wav, 1, s_nokia_wav_size, wf);
    fclose(wf);
}

/* ---- On-demand execution via AT+CLAW=speaker[,sine|nokia] ---- */

typedef struct {
    char             *script;  /* heap-allocated; freed after task completes */
    SemaphoreHandle_t done;
} speaker_task_arg_t;

static void speaker_lua_task(void *param)
{
    speaker_task_arg_t *arg = (speaker_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[speaker] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[speaker] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[speaker] runtime error: %s\n", lua_tostring(L, -1));
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

/* mode: "sine", "nokia", or "" / NULL for all steps.
 * vol:  volume string "0.0"-"1.0"; NULL or "" uses default "0.2". */
void lua_speaker_run(const char *mode, const char *vol)
{
    if (!mode || mode[0] == '\0') {
        mode = "all";
    }
    if (!vol || vol[0] == '\0') {
        vol = "0.2";
    }

    char preamble[128];
    preamble[0] = '\0';
    strlcat(preamble, "SPEAKER_MODE='", sizeof(preamble));
    strlcat(preamble, mode,             sizeof(preamble));
    strlcat(preamble, "'\nSPEAKER_VOL='", sizeof(preamble));
    strlcat(preamble, vol,              sizeof(preamble));
    strlcat(preamble, "'\n",            sizeof(preamble));

    size_t total = strlen(preamble) + strlen(s_speaker_script) + 1;
    char *script = malloc(total);
    if (!script) {
        printf("[speaker] malloc failed\n");
        return;
    }
    strcpy(script, preamble);
    strcat(script, s_speaker_script);

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[speaker] semaphore create failed\n");
        free(script);
        return;
    }

    speaker_task_arg_t *arg = (speaker_task_arg_t *)malloc(sizeof(speaker_task_arg_t));
    if (!arg) {
        printf("[speaker] malloc failed\n");
        free(script);
        vSemaphoreDelete(done);
        return;
    }
    arg->script = script;
    arg->done   = done;

    if (rtos_task_create(NULL, "speaker_lua", speaker_lua_task, arg,
                         16384, 1) != RTK_SUCCESS) {
        printf("[speaker] task create failed\n");
        free(script);
        free(arg);
        vSemaphoreDelete(done);
        return;
    }

    /* Timeout: sine(20s) + nokia(~3s) + overhead = 30s max */
    xSemaphoreTake(done, pdMS_TO_TICKS(60000));
    vSemaphoreDelete(done);
}
