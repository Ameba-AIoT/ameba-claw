/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AT+CLAW=gpio_ctrl — GPIO button-stimulus for the automated test bench.
 *
 * A TESTER board (same image as the DUT, GPIO pins named one-to-one with
 * the DUT) drives a pin that is physically wired to a DUT button line.
 * Open-drain emulation: "press" = drive the pin LOW (sink current, beats
 * the DUT's internal pull-up); "release" = switch to INPUT/hi-Z so the
 * DUT pull-up restores high. The pin is NEVER driven high, so it can never
 * fight the physical switch or the pull-up. Both boards must share GND.
 *
 *   AT+CLAW=gpio_ctrl,<pin>,click           single click (CLICK_MS press)
 *   AT+CLAW=gpio_ctrl,<pin>,double          double click
 *   AT+CLAW=gpio_ctrl,<pin>,long[,<ms>]     long press (default LONG_MS)
 *   AT+CLAW=gpio_ctrl,<pin>,bounce          contact-bounce burst then settle
 *   AT+CLAW=gpio_ctrl,<pin>,press           hold down (stays low until release)
 *   AT+CLAW=gpio_ctrl,<pin>,release         release (back to hi-Z)
 *   AT+CLAW=gpio_ctrl,<pin>,seq,<d0>,<d1>…  custom ms sequence: from "press",
 *                                           alternate press/release each d_i,
 *                                           always ends released
 *
 * <pin> is a name like PA_22 (case-insensitive), matching board.json.
 */

#include "ameba_claw_defs.h"   /* CLAW_AGENT_AUTO_TEST gate + CLAW_GPIOCTRL_* */

#if CLAW_AGENT_AUTO_TEST

#include "ameba_soc.h"
#include "atcmd_service.h"
#include "atcmd_handlers.h"
#include <string.h>
#include <stdlib.h>

/* Tracks which pins have had GPIO_Init() run (one-time pinmux + clock). */
static uint8_t s_gpioctrl_inited[PIN_TOTAL_NUM];

/* Parse "PA_22" / "pb8" → PinName value ((port<<5)|pin), or -1 on error.
 * Mirrors lua/modules/luhw.h::luhw_check_pin so pin names stay consistent. */
static int gpio_ctrl_parse_pin(const char *s)
{
    if (!s || (s[0] != 'p' && s[0] != 'P')) {
        return -1;
    }
    int port_val;
    if (s[1] == 'a' || s[1] == 'A')      { port_val = 0; }
    else if (s[1] == 'b' || s[1] == 'B') { port_val = 1; }
    else if (s[1] == 'c' || s[1] == 'C') { port_val = 2; }
    else { return -1; }

    if (s[2] != '_' && s[2] != '\0') {
        return -1;
    }
    const char *num_str = (s[2] == '_') ? &s[3] : &s[2];
    char *end;
    long pin = strtol(num_str, &end, 10);
    if (*end != '\0' || pin < 0 || pin > 31) {
        return -1;
    }
    if (port_val == 2 && pin > 8) {   /* PC supports 0-8 only */
        return -1;
    }
    return (port_val << 5) | (int)pin;
}

/* Init a pin to the idle state: INPUT / hi-Z, output latch pre-loaded LOW
 * so a later direction-flip to OUTPUT drives low (never high). */
static void gpio_ctrl_ensure_init(u32 pin)
{
    int idx = (int)pin;
    if (idx < 0 || idx >= PIN_TOTAL_NUM) {
        return;
    }
    if (s_gpioctrl_inited[idx]) {
        return;
    }
    RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);
    GPIO_InitTypeDef init;
    init.GPIO_Pin  = pin;
    init.GPIO_Mode = GPIO_Mode_IN;
    init.GPIO_PuPd = GPIO_PuPd_NOPULL;   /* DUT supplies its own pull-up */
    GPIO_Init(&init);
    GPIO_WriteBit(pin, 0);               /* latch low for open-drain emulation */
    s_gpioctrl_inited[idx] = 1;
}

static void gpio_ctrl_press(u32 pin)    /* sink low */
{
    GPIO_WriteBit(pin, 0);
    GPIO_Direction(pin, GPIO_Mode_OUT);
}

static void gpio_ctrl_release(u32 pin)  /* hi-Z, DUT pull-up restores high */
{
    GPIO_Direction(pin, GPIO_Mode_IN);
}

static void gpio_ctrl_delay(int ms)
{
    if (ms < 0) {
        ms = 0;
    }
    if (ms > CLAW_GPIOCTRL_MAX_MS) {
        ms = CLAW_GPIOCTRL_MAX_MS;
    }
    rtos_time_delay_ms((uint32_t)ms);
}

/* One press pulse of the given width, then release. */
static void gpio_ctrl_pulse(u32 pin, int press_ms)
{
    gpio_ctrl_press(pin);
    gpio_ctrl_delay(press_ms);
    gpio_ctrl_release(pin);
}

/* Button pins wired from TESTER to DUT — names from ameba_pinmux.h. */
static const u32 k_tester_btn_pins[] = {
    _PA_22,   /* btn_a */
    _PA_23,   /* btn_b */
    _PA_24,   /* btn_c */
    _PA_29,   /* btn_d */
};

/* Pull all button lines HIGH at boot so the TESTER does not sink the DUT
 * input lines before the first gpio_ctrl command arrives.
 * s_gpioctrl_inited[] is intentionally left clear so gpio_ctrl_ensure_init()
 * will reconfigure to NOPULL (open-drain idle) on first actual use. */
void claw_gpio_ctrl_startup_pullup(void)
{
    RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);
    GPIO_InitTypeDef init;
    int n = (int)(sizeof(k_tester_btn_pins) / sizeof(k_tester_btn_pins[0]));
    for (int i = 0; i < n; i++) {
        init.GPIO_Pin  = k_tester_btn_pins[i];
        init.GPIO_Mode = GPIO_Mode_IN;
        init.GPIO_PuPd = GPIO_PuPd_NOPULL;
        GPIO_Init(&init);
        RTK_LOGI(NOTAG, "gpio_ctrl startup pullup pin=%08x\n", k_tester_btn_pins[i]);
    }
}

void handle_cmd_gpio_ctrl(u16 argc, char **argv, const char *arg2, const char *arg3)
{
    const char *pin_s   = arg2;          /* argv[2] */
    const char *gesture = arg3;          /* argv[3] */

    if (!pin_s[0] || !gesture[0]) {
        at_printf("\r\n+CLAW:usage: AT+CLAW=gpio_ctrl,<pin>,"
                  "<click|double|long[,ms]|bounce|press|release|seq,d0,d1,...>\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
        return;
    }

    int pin = gpio_ctrl_parse_pin(pin_s);
    if (pin < 0) {
        at_printf("\r\n+CLAW:gpio_ctrl,bad pin '%s' (expected e.g. PA_22)\r\n", pin_s);
        at_printf(ATCMD_ERROR_END_STR, 1);
        return;
    }
    u32 gpio = (u32)pin;
    gpio_ctrl_ensure_init(gpio);

    if (strcmp(gesture, "click") == 0) {
        gpio_ctrl_pulse(gpio, CLAW_GPIOCTRL_CLICK_MS);

    } else if (strcmp(gesture, "double") == 0) {
        gpio_ctrl_pulse(gpio, CLAW_GPIOCTRL_CLICK_MS);
        gpio_ctrl_delay(CLAW_GPIOCTRL_DOUBLE_GAP_MS);
        gpio_ctrl_pulse(gpio, CLAW_GPIOCTRL_CLICK_MS);

    } else if (strcmp(gesture, "long") == 0) {
        int ms = (argc >= 5 && argv[4] && argv[4][0]) ? atoi(argv[4]) : CLAW_GPIOCTRL_LONG_MS;
        gpio_ctrl_pulse(gpio, ms);

    } else if (strcmp(gesture, "bounce") == 0) {
        /* Emit BOUNCE_COUNT short edges (chattering contact) then settle
         * pressed for CLICK_MS — exercises the DUT's debounce path. */
        for (int i = 0; i < CLAW_GPIOCTRL_BOUNCE_COUNT; i++) {
            gpio_ctrl_press(gpio);
            gpio_ctrl_delay(CLAW_GPIOCTRL_BOUNCE_EDGE_MS);
            gpio_ctrl_release(gpio);
            gpio_ctrl_delay(CLAW_GPIOCTRL_BOUNCE_EDGE_MS);
        }
        gpio_ctrl_press(gpio);
        gpio_ctrl_delay(CLAW_GPIOCTRL_CLICK_MS);
        gpio_ctrl_release(gpio);

    } else if (strcmp(gesture, "press") == 0) {
        gpio_ctrl_press(gpio);            /* stays low until an explicit release */

    } else if (strcmp(gesture, "release") == 0) {
        gpio_ctrl_release(gpio);

    } else if (strcmp(gesture, "seq") == 0) {
        /* Durations start at argv[4]; alternate press/release, ensure released. */
        int n = 0;
        int pressed = 0;
        for (int i = 4; i < argc && argv[i] && argv[i][0] && n < CLAW_GPIOCTRL_SEQ_MAX; i++, n++) {
            int ms = atoi(argv[i]);
            if (pressed) {
                gpio_ctrl_release(gpio);
            } else {
                gpio_ctrl_press(gpio);
            }
            pressed = !pressed;
            gpio_ctrl_delay(ms);
        }
        if (n == 0) {
            at_printf("\r\n+CLAW:gpio_ctrl,seq needs at least one duration\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }
        gpio_ctrl_release(gpio);          /* always end released */

    } else {
        at_printf("\r\n+CLAW:gpio_ctrl,unknown gesture '%s' "
                  "(click|double|long|bounce|press|release|seq)\r\n", gesture);
        at_printf(ATCMD_ERROR_END_STR, 1);
        return;
    }

    at_printf("\r\n+CLAW:gpio_ctrl,%s,%s,done\r\n", pin_s, gesture);
    at_printf(ATCMD_OK_END_STR);
}

#endif /* CLAW_AGENT_AUTO_TEST */
