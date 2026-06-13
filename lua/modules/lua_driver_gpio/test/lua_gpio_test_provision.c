/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_gpio_test_provision.c — Writes test_gpio.lua to VFS at boot.
**
** Kept separate from lua_driver_gpio.c so the driver stays free of test code.
** The embedded string MUST stay in sync with test/test_gpio.lua.
*/

#include <stdio.h>
#include <string.h>

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static const char s_test_script[] =
	"-- test_gpio.lua\n"
	"-- GPIO test: PA30 (output) -> PA31 (input / interrupt, loopback)\n"
	"-- Covers all 9 APIs incl. set_pull, get_irq_count, clear_irq_count\n"
	"\n"
	"local gpio = require(\"gpio\")\n"
	"local sys  = require(\"sys\")\n"
	"\n"
	"local OUT_PIN = \"PA_30\"\n"
	"local INT_PIN = \"PA_31\"\n"
	"\n"
	"local fail_count = 0\n"
	"\n"
	"local function check_val(label, got, expected)\n"
	"    if got == expected then\n"
	"        print(\"[gpio] \" .. label .. \": ok (val=\" .. tostring(got) .. \")\")\n"
	"    else\n"
	"        print(\"[gpio] \" .. label .. \": FAIL got=\" .. tostring(got) .. \" expected=\" .. tostring(expected))\n"
	"        fail_count = fail_count + 1\n"
	"    end\n"
	"end\n"
	"\n"
	"local function check_ge(label, got, minv)\n"
	"    if got >= minv then\n"
	"        print(\"[gpio] \" .. label .. \": ok (count=\" .. tostring(got) .. \" >= \" .. tostring(minv) .. \")\")\n"
	"    else\n"
	"        print(\"[gpio] \" .. label .. \": FAIL count=\" .. tostring(got) .. \" expected >= \" .. tostring(minv))\n"
	"        fail_count = fail_count + 1\n"
	"    end\n"
	"end\n"
	"\n"
	"gpio.set_direction(OUT_PIN, \"output\")\n"
	"gpio.set_level(OUT_PIN, 0)\n"
	"sys.sleep_ms(10)\n"
	"\n"
	"print(\"[gpio] test 0a: get_level on output\")\n"
	"gpio.set_level(OUT_PIN, 0)\n"
	"sys.sleep_ms(5)\n"
	"check_val(\"get_level low\",  gpio.get_level(OUT_PIN), 0)\n"
	"gpio.set_level(OUT_PIN, 1)\n"
	"sys.sleep_ms(5)\n"
	"check_val(\"get_level high\", gpio.get_level(OUT_PIN), 1)\n"
	"gpio.set_level(OUT_PIN, 0)\n"
	"sys.sleep_ms(5)\n"
	"\n"
	"print(\"[gpio] test 0b: set_direction input + get_level loopback\")\n"
	"gpio.set_direction(INT_PIN, \"input\")\n"
	"gpio.set_level(OUT_PIN, 1)\n"
	"sys.sleep_ms(5)\n"
	"check_val(\"input reads high\", gpio.get_level(INT_PIN), 1)\n"
	"gpio.set_level(OUT_PIN, 0)\n"
	"sys.sleep_ms(5)\n"
	"check_val(\"input reads low\",  gpio.get_level(INT_PIN), 0)\n"
	"\n"
	"print(\"[gpio] test 0c: set_pull up/down/none\")\n"
	"gpio.set_direction(OUT_PIN, \"input\")\n"
	"gpio.set_direction(INT_PIN, \"input\")\n"
	"gpio.set_pull(INT_PIN, \"up\")\n"
	"sys.sleep_ms(10)\n"
	"check_val(\"pull up reads high\", gpio.get_level(INT_PIN), 1)\n"
	"gpio.set_pull(INT_PIN, \"down\")\n"
	"sys.sleep_ms(10)\n"
	"check_val(\"pull down reads low\", gpio.get_level(INT_PIN), 0)\n"
	"gpio.set_pull(INT_PIN, \"none\")\n"
	"gpio.set_direction(OUT_PIN, \"output\")\n"
	"gpio.set_level(OUT_PIN, 0)\n"
	"sys.sleep_ms(5)\n"
	"\n"
	"print(\"[gpio_irq] test 1: rising edge x10\")\n"
	"gpio.set_level(OUT_PIN, 0)\n"
	"sys.sleep_ms(5)\n"
	"gpio.set_irq(INT_PIN, \"rising\", 1)\n"
	"gpio.clear_irq_count(INT_PIN)\n"
	"gpio.irq_enable(INT_PIN)\n"
	"sys.sleep_ms(5)\n"
	"for i = 1, 10 do\n"
	"    gpio.set_level(OUT_PIN, 1)\n"
	"    sys.sleep_ms(20)\n"
	"    gpio.set_level(OUT_PIN, 0)\n"
	"    sys.sleep_ms(10)\n"
	"end\n"
	"gpio.irq_disable(INT_PIN)\n"
	"check_ge(\"rising edge x10\", gpio.get_irq_count(INT_PIN), 5)\n"
	"\n"
	"print(\"[gpio_irq] test 2: falling edge x10\")\n"
	"gpio.set_level(OUT_PIN, 1)\n"
	"sys.sleep_ms(5)\n"
	"gpio.set_irq(INT_PIN, \"falling\", 1)\n"
	"gpio.clear_irq_count(INT_PIN)\n"
	"gpio.irq_enable(INT_PIN)\n"
	"sys.sleep_ms(5)\n"
	"for i = 1, 10 do\n"
	"    gpio.set_level(OUT_PIN, 0)\n"
	"    sys.sleep_ms(20)\n"
	"    gpio.set_level(OUT_PIN, 1)\n"
	"    sys.sleep_ms(10)\n"
	"end\n"
	"gpio.irq_disable(INT_PIN)\n"
	"check_ge(\"falling edge x10\", gpio.get_irq_count(INT_PIN), 5)\n"
	"\n"
	"print(\"[gpio_irq] test 3: both edges x10\")\n"
	"gpio.set_level(OUT_PIN, 0)\n"
	"sys.sleep_ms(5)\n"
	"gpio.set_irq(INT_PIN, \"both\", 1)\n"
	"gpio.clear_irq_count(INT_PIN)\n"
	"gpio.irq_enable(INT_PIN)\n"
	"sys.sleep_ms(5)\n"
	"for i = 1, 10 do\n"
	"    gpio.set_level(OUT_PIN, 1)\n"
	"    sys.sleep_ms(20)\n"
	"    gpio.set_level(OUT_PIN, 0)\n"
	"    sys.sleep_ms(20)\n"
	"end\n"
	"gpio.irq_disable(INT_PIN)\n"
	"check_ge(\"both edges x10\", gpio.get_irq_count(INT_PIN), 10)\n"
	"\n"
	"print(\"[gpio_irq] test 4: level high x10\")\n"
	"gpio.set_level(OUT_PIN, 0)\n"
	"sys.sleep_ms(5)\n"
	"gpio.set_irq(INT_PIN, \"level_high\", 1)\n"
	"gpio.clear_irq_count(INT_PIN)\n"
	"gpio.irq_enable(INT_PIN)\n"
	"for i = 1, 10 do\n"
	"    gpio.set_level(OUT_PIN, 1)\n"
	"    sys.sleep_ms(20)\n"
	"    gpio.set_level(OUT_PIN, 0)\n"
	"    sys.sleep_ms(20)\n"
	"end\n"
	"gpio.irq_disable(INT_PIN)\n"
	"check_ge(\"level high x10\", gpio.get_irq_count(INT_PIN), 1)\n"
	"\n"
	"print(\"[gpio_irq] test 5: level low x10\")\n"
	"gpio.set_level(OUT_PIN, 1)\n"
	"sys.sleep_ms(5)\n"
	"gpio.set_irq(INT_PIN, \"level_low\", 1)\n"
	"gpio.clear_irq_count(INT_PIN)\n"
	"gpio.irq_enable(INT_PIN)\n"
	"for i = 1, 10 do\n"
	"    gpio.set_level(OUT_PIN, 0)\n"
	"    sys.sleep_ms(20)\n"
	"    gpio.set_level(OUT_PIN, 1)\n"
	"    sys.sleep_ms(20)\n"
	"end\n"
	"gpio.irq_disable(INT_PIN)\n"
	"check_ge(\"level low x10\", gpio.get_irq_count(INT_PIN), 1)\n"
	"\n"
	"print(\"[gpio_irq] test 6: resource recycle\")\n"
	"gpio.set_level(OUT_PIN, 0)\n"
	"sys.sleep_ms(5)\n"
	"gpio.set_irq(INT_PIN, \"rising\", 1)\n"
	"gpio.clear_irq_count(INT_PIN)\n"
	"gpio.irq_enable(INT_PIN)\n"
	"sys.sleep_ms(5)\n"
	"for i = 1, 5 do\n"
	"    gpio.set_level(OUT_PIN, 1)\n"
	"    sys.sleep_ms(20)\n"
	"    gpio.set_level(OUT_PIN, 0)\n"
	"    sys.sleep_ms(10)\n"
	"end\n"
	"gpio.irq_disable(INT_PIN)\n"
	"check_ge(\"recycle re-fire\", gpio.get_irq_count(INT_PIN), 3)\n"
	"gpio.clear_irq_count(INT_PIN)\n"
	"check_val(\"recycle counter cleared\", gpio.get_irq_count(INT_PIN), 0)\n"
	"\n"
	"if fail_count == 0 then\n"
	"    print(\"success\")\n"
	"else\n"
	"    print(\"FAIL: \" .. fail_count .. \" test(s) failed\")\n"
	"end\n";

void lua_driver_gpio_provision(void)
{
	const char *path = "vfs:test_gpio.lua";
	{ FILE *_ck = fopen(path, "r"); if (_ck) { fclose(_ck); return; } }

	FILE *f = fopen(path, "w");
	if (f == NULL) {
		return;
	}
	fwrite(s_test_script, 1, strlen(s_test_script), f);
	fclose(f);
}

/* ── On-demand execution via AT+CLAW=gpio ── */

typedef struct {
	const char       *script;
	SemaphoreHandle_t done;
} gpio_task_arg_t;

static void gpio_lua_task(void *param)
{
	gpio_task_arg_t *arg = (gpio_task_arg_t *)param;

	lua_State *L = luaL_newstate();
	if (!L) {
		printf("[gpio] failed to create Lua state\n");
	} else {
		luaL_openlibs(L);
		if (luaL_loadstring(L, arg->script) != LUA_OK) {
			printf("[gpio] parse error: %s\n", lua_tostring(L, -1));
			lua_pop(L, 1);
		} else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
			printf("[gpio] runtime error: %s\n", lua_tostring(L, -1));
			lua_pop(L, 1);
		}
		lua_close(L);
	}

	xSemaphoreGive(arg->done);
	rtos_task_delete(NULL);
}

void lua_gpio_run(void)
{
	SemaphoreHandle_t done = xSemaphoreCreateBinary();
	if (!done) {
		printf("[gpio] semaphore create failed\n");
		return;
	}

	gpio_task_arg_t arg = { .script = s_test_script, .done = done };

	if (rtos_task_create(NULL, "gpio_lua_task", gpio_lua_task, &arg,
	                     16384, 1) != RTK_SUCCESS) {
		printf("[gpio] task create failed\n");
		vSemaphoreDelete(done);
		return;
	}

	xSemaphoreTake(done, portMAX_DELAY);
	vSemaphoreDelete(done);
}
