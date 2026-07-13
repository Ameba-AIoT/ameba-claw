/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * display_lcdc_test.c — Pure-C LCDC RGB888 480x480 (ST7701P) colour-fill test.
 *
 * Trigger: AT+CLAW=display_lcdc
 *
 * This is a self-contained C port of the Lua test
 * (lua_driver_lcdc/test/test_lcdc_rgb_st7701p_touch_gt711.lua +
 *  lua_lcdc_test_provision.c), reduced to a simple three-colour flush
 * (red / green / blue) with no touch IC.  Everything — SPI panel init,
 * panel reset, backlight, pinmux, LCDC RGB configuration, framebuffer
 * fill and teardown — is done directly in C, no Lua runtime involved.
 *
 * Board: st7701p_rgb_480x480 on RTL8721F eval board.
 *
 * Pin mapping (matches the ST7701P Lua test):
 *   Panel RESET : PA_23
 *   Backlight   : PA_25 (active high)
 *   SPI1 init   : CS=PA_20 SCLK=PA_21 MOSI=PA_22 (9-bit Mode3 5 MHz)
 *   RGB888 bus  : D0..D23 + HSYNC/VSYNC/DCLK/DE (see s_pinmux[] below)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ameba_soc.h"
#include "ameba_lcdc.h"
#include "spi_api.h"

#include "os_wrapper.h"
#include "FreeRTOS.h"
#include "semphr.h"

/* ---- Panel geometry (RGB888, 3 bytes/pixel) ---- */
#define PANEL_W        480
#define PANEL_H        480
#define PANEL_BPP      3
#define PANEL_FB_BYTES (PANEL_W * PANEL_H * PANEL_BPP)

/* ---- Board GPIOs ---- */
#define PIN_PANEL_RST  _PA_23
#define PIN_BACKLIGHT  _PA_25

/* ---- Framebuffer ---- */
static uint32_t s_fb_addr;

static uint32_t fb_alloc(void)
{
    void *buf = rtos_mem_malloc(PANEL_FB_BYTES);
    if (!buf) {
        return 0;
    }
    memset(buf, 0, PANEL_FB_BYTES);
    DCache_Clean((uint32_t)buf, PANEL_FB_BYTES);
    return (uint32_t)buf;
}

/* Fill the whole RGB888 framebuffer with a single colour. */
static void fb_fill(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *p = (uint8_t *)s_fb_addr;
    for (uint32_t i = 0; i < (uint32_t)(PANEL_W * PANEL_H); i++) {
        p[i * 3]     = r;
        p[i * 3 + 1] = g;
        p[i * 3 + 2] = b;
    }
    DCache_Clean(s_fb_addr, PANEL_FB_BYTES);
}

/* ---- ST7701P 9-bit SPI register init ------------------------------------ */
/* The ST7701P needs a one-shot 9-bit SPI register config before the RGB
 * interface starts.  9-bit frames are unavailable to the Lua SPI driver, so
 * this stays in C.  Sequence ported verbatim from the Lua test provision. */

static spi_t s_spi;

static void spi_cmd(uint16_t cmd)  { spi_master_write(&s_spi, (int)cmd); }
static void spi_dat(uint16_t data) { spi_master_write(&s_spi, (int)(data | 0x100u)); }

static void st7701p_send_init_cmds(void)
{
    spi_cmd(0xFF); spi_dat(0x77); spi_dat(0x01);
    spi_dat(0x00); spi_dat(0x00); spi_dat(0x13);
    spi_cmd(0xEF); spi_dat(0x08);
    spi_cmd(0xFF); spi_dat(0x77); spi_dat(0x01);
    spi_dat(0x00); spi_dat(0x00); spi_dat(0x10);
    spi_cmd(0xC0); spi_dat(0x3B); spi_dat(0x00);
    spi_cmd(0xC1); spi_dat(0x0D); spi_dat(0x02);
    spi_cmd(0xC2); spi_dat(0x37); spi_dat(0x08);
    spi_cmd(0xC7); spi_dat(0x00);
    spi_cmd(0xCC); spi_dat(0x18);
    spi_cmd(0xB0);
    spi_dat(0x00); spi_dat(0x11); spi_dat(0x17);
    spi_dat(0x0E); spi_dat(0x12); spi_dat(0x06);
    spi_dat(0x06); spi_dat(0x08); spi_dat(0x08);
    spi_dat(0x20); spi_dat(0x04); spi_dat(0x11);
    spi_dat(0x0F); spi_dat(0x29); spi_dat(0x30);
    spi_dat(0x1F);
    spi_cmd(0xB1);
    spi_dat(0x00); spi_dat(0x13); spi_dat(0x18);
    spi_dat(0x0F); spi_dat(0x12); spi_dat(0x07);
    spi_dat(0x06); spi_dat(0x08); spi_dat(0x07);
    spi_dat(0x21); spi_dat(0x04); spi_dat(0x12);
    spi_dat(0x10); spi_dat(0x29); spi_dat(0x34);
    spi_dat(0x1F);
    spi_cmd(0xFF); spi_dat(0x77); spi_dat(0x01);
    spi_dat(0x00); spi_dat(0x00); spi_dat(0x11);
    spi_cmd(0xB0); spi_dat(0x60);
    spi_cmd(0xB1); spi_dat(0x32);
    spi_cmd(0xB2); spi_dat(0x8A);
    spi_cmd(0xB3); spi_dat(0x80);
    spi_cmd(0xB5); spi_dat(0x4B);
    spi_cmd(0xB7); spi_dat(0x85);
    spi_cmd(0xB8); spi_dat(0x21);
    spi_cmd(0xC0); spi_dat(0x07);
    spi_cmd(0xC1); spi_dat(0x78);
    spi_cmd(0xC2); spi_dat(0x78);
    spi_cmd(0xE0); spi_dat(0x00); spi_dat(0x1B); spi_dat(0x02);
    spi_cmd(0xE1);
    spi_dat(0x08); spi_dat(0xA0); spi_dat(0x00); spi_dat(0x00);
    spi_dat(0x07); spi_dat(0xA0); spi_dat(0x00); spi_dat(0x00);
    spi_dat(0x00); spi_dat(0x44); spi_dat(0x44);
    spi_cmd(0xE2);
    spi_dat(0x11); spi_dat(0x11); spi_dat(0x44); spi_dat(0x44);
    spi_dat(0xED); spi_dat(0xA0); spi_dat(0x00); spi_dat(0x00);
    spi_dat(0xEC); spi_dat(0xA0); spi_dat(0x00); spi_dat(0x00);
    spi_cmd(0xE3);
    spi_dat(0x00); spi_dat(0x00); spi_dat(0x11); spi_dat(0x11);
    spi_cmd(0xE4); spi_dat(0x44); spi_dat(0x44);
    spi_cmd(0xE5);
    spi_dat(0x0A); spi_dat(0xE9); spi_dat(0xD8); spi_dat(0xA0);
    spi_dat(0x0C); spi_dat(0xEB); spi_dat(0xD8); spi_dat(0xA0);
    spi_dat(0x0E); spi_dat(0xED); spi_dat(0xD8); spi_dat(0xA0);
    spi_dat(0x10); spi_dat(0xEF); spi_dat(0xD8); spi_dat(0xA0);
    spi_cmd(0xE6);
    spi_dat(0x00); spi_dat(0x00); spi_dat(0x11); spi_dat(0x11);
    spi_cmd(0xE7); spi_dat(0x44); spi_dat(0x44);
    spi_cmd(0xE8);
    spi_dat(0x09); spi_dat(0xE8); spi_dat(0xD8); spi_dat(0xA0);
    spi_dat(0x0B); spi_dat(0xEA); spi_dat(0xD8); spi_dat(0xA0);
    spi_dat(0x0D); spi_dat(0xEC); spi_dat(0xD8); spi_dat(0xA0);
    spi_dat(0x0F); spi_dat(0xEE); spi_dat(0xD8); spi_dat(0xA0);
    spi_cmd(0xEB);
    spi_dat(0x02); spi_dat(0x00); spi_dat(0xE4); spi_dat(0xE4);
    spi_dat(0x88); spi_dat(0x00); spi_dat(0x40);
    spi_cmd(0xEC); spi_dat(0x3C); spi_dat(0x00);
    spi_cmd(0xED);
    spi_dat(0xAB); spi_dat(0x89); spi_dat(0x76); spi_dat(0x54);
    spi_dat(0x02); spi_dat(0xFF); spi_dat(0xFF); spi_dat(0xFF);
    spi_dat(0xFF); spi_dat(0xFF); spi_dat(0xFF); spi_dat(0x20);
    spi_dat(0x45); spi_dat(0x67); spi_dat(0x98); spi_dat(0xBA);
    spi_cmd(0xEF);
    spi_dat(0x08); spi_dat(0x08); spi_dat(0x08);
    spi_dat(0x45); spi_dat(0x3F); spi_dat(0x54);
    spi_cmd(0xFF); spi_dat(0x77); spi_dat(0x01);
    spi_dat(0x00); spi_dat(0x00); spi_dat(0x13);
    spi_cmd(0xE8); spi_dat(0x00); spi_dat(0x0E);
    spi_cmd(0xFF); spi_dat(0x77); spi_dat(0x01);
    spi_dat(0x00); spi_dat(0x00); spi_dat(0x00);
    spi_cmd(0x11);
    rtos_time_delay_ms(120);
    spi_cmd(0xFF); spi_dat(0x77); spi_dat(0x01);
    spi_dat(0x00); spi_dat(0x00); spi_dat(0x13);
    spi_cmd(0xE8); spi_dat(0x00); spi_dat(0x0C);
    rtos_time_delay_ms(120);
    spi_cmd(0xE8); spi_dat(0x00); spi_dat(0x00);
    spi_cmd(0xFF); spi_dat(0x77); spi_dat(0x01);
    spi_dat(0x00); spi_dat(0x00); spi_dat(0x00);
    spi_cmd(0x29);
    spi_cmd(0x36); spi_dat(0x00);
}

static void st7701p_hw_init(void)
{
    /* --- Panel RESET: PA_23 --- */
    GPIO_InitTypeDef g = {0};
    g.GPIO_Pin  = PIN_PANEL_RST;
    g.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_Init(&g);

    GPIO_WriteBit(PIN_PANEL_RST, 1);
    rtos_time_delay_ms(4);
    GPIO_WriteBit(PIN_PANEL_RST, 0);
    rtos_time_delay_ms(30);
    GPIO_WriteBit(PIN_PANEL_RST, 1);
    rtos_time_delay_ms(120);

    /* --- SPI1: CS=PA_20, SCLK=PA_21, MOSI=PA_22, 9-bit Mode3 5 MHz --- */
    s_spi.spi_idx = MBED_SPI1;
    spi_init(&s_spi, _PA_22, 0xFFFFFFFF, _PA_21, _PA_20);
    spi_frequency(&s_spi, 5000000);
    spi_format(&s_spi, 9, 3, 0);

    st7701p_send_init_cmds();
}

/* ---- LCDC signal pinmux (st7701p_rgb_480x480) --------------------------- */
/* Data lines are configured D0..D23 via PINMUX_FUNCTION_LCD_D0 + index.
 * Pin encoding: PA_X = X, PB_X = 0x20+X, PC_X = 0x40+X (raw SDK pin numbers). */

static const uint32_t s_data_pins[24] = {
    0x10, 0x0F, 0x0E, 0x0D,  /* D0..D3  : PA16,PA15,PA14,PA13 */
    0x0C, 0x41, 0x40, 0x3F,  /* D4..D7  : PA12,PC1, PC0, PB31 */
    0x3E, 0x3D, 0x3C, 0x3B,  /* D8..D11 : PB30,PB29,PB28,PB27 */
    0x3A, 0x39, 0x38, 0x37,  /* D12..D15: PB26,PB25,PB24,PB23 */
    0x36, 0x35, 0x33, 0x32,  /* D16..D19: PB22,PB21,PB19,PB18 */
    0x31, 0x30, 0x2F, 0x2E,  /* D20..D23: PB17,PB16,PB15,PB14 */
};

static void lcdc_pinmux(void)
{
    for (int i = 0; i < 24; i++) {
        Pinmux_Config(s_data_pins[i], PINMUX_FUNCTION_LCD_D0 + (uint32_t)i);
    }
    Pinmux_Config(0x13, PINMUX_FUNCTION_LCD_RGB_HSYNC);  /* PA19 */
    Pinmux_Config(0x12, PINMUX_FUNCTION_LCD_RGB_VSYNC);  /* PA18 */
    Pinmux_Config(0x2D, PINMUX_FUNCTION_LCD_RGB_DCLK);   /* PB13 */
    Pinmux_Config(0x11, PINMUX_FUNCTION_LCD_RGB_DE);     /* PA17 */
}

/* ---- LCDC IRQ: clear all pending interrupts ---- */
static void lcdc_irq_handler(void)
{
    uint32_t ints = LCDC_GetINTStatus(LCDC);
    LCDC_ClearINT(LCDC, ints);
}

/* Configure LCDC in RGB888 mode (continuous DMA refresh).
 * Timing/polarity from panel_st7701p_rgb.c panel_timing_t. */
static void lcdc_rgb_init(void)
{
    LCDC_RccEnable();

    InterruptRegister((IRQ_FUN)lcdc_irq_handler, LCDC_IRQ, (u32)LCDC, INT_PRI_MIDDLE);
    InterruptEn(LCDC_IRQ, INT_PRI_MIDDLE);

    LCDC_RGBInitTypeDef rgb;
    LCDC_Cmd(LCDC, DISABLE);
    LCDC_RGBStructInit(&rgb);

    rgb.Panel_RgbTiming.RgbVsw = 3;
    rgb.Panel_RgbTiming.RgbVbp = 12;
    rgb.Panel_RgbTiming.RgbVfp = 15;
    rgb.Panel_RgbTiming.RgbHsw = 2;
    rgb.Panel_RgbTiming.RgbHbp = 2;
    rgb.Panel_RgbTiming.RgbHfp = 15;

    rgb.Panel_Init.IfWidth        = LCDC_RGB_IF_24_BIT;
    rgb.Panel_Init.ImgWidth       = PANEL_W;
    rgb.Panel_Init.ImgHeight      = PANEL_H;
    rgb.Panel_Init.InputFormat    = LCDC_INPUT_FORMAT_RGB888;
    rgb.Panel_Init.OutputFormat   = LCDC_OUTPUT_FORMAT_RGB888;
    rgb.Panel_Init.RGBRefreshFreq = 60;

    rgb.Panel_RgbTiming.Flags.RgbEnPolar      = LCDC_RGB_EN_PUL_HIGH_LEV_ACTIVE;   /* DE active high */
    rgb.Panel_RgbTiming.Flags.RgbDclkActvEdge = LCDC_RGB_DCLK_RISING_EDGE_FETCH;   /* rising edge */
    rgb.Panel_RgbTiming.Flags.RgbHsPolar      = LCDC_RGB_HS_PUL_LOW_LEV_SYNC;      /* HSYNC low */
    rgb.Panel_RgbTiming.Flags.RgbVsPolar      = LCDC_RGB_VS_PUL_LOW_LEV_SYNC;      /* VSYNC low */

    LCDC_RGBInit(LCDC, &rgb);

    LCDC_DMABurstSizeConfig(LCDC, LCDC_DMA_BURSTSIZE_2X64BYTES);
    LCDC_DMAImgCfg(LCDC, s_fb_addr);

    LCDC_LineINTPosConfig(LCDC, PANEL_H / 2);
    LCDC_INTConfig(LCDC,
        LCDC_BIT_LCD_FRD_INTEN | LCDC_BIT_DMA_UN_INTEN | LCDC_BIT_LCD_LIN_INTEN, ENABLE);

    LCDC_Cmd(LCDC, ENABLE);
}

static void lcdc_reload(void)
{
    LCDC_ShadowReloadConfig(LCDC);   /* push new framebuffer contents */
}

static void lcdc_teardown(void)
{
    /* Mirror lua_lcdc_deinit(): mask IRQs, let in-flight DMA burst finish,
     * disable output, drain write buffer.  Do NOT call LCDC_DeInit() — it can
     * trigger an imprecise bus fault while a PSRAM DMA burst is completing. */
    LCDC_INTConfig(LCDC,
        LCDC_BIT_LCD_FRD_INTEN | LCDC_BIT_DMA_UN_INTEN |
        LCDC_BIT_LCD_LIN_INTEN | LCDC_BIT_FRM_START_INTEN, DISABLE);
    LCDC_ClearINT(LCDC, LCDC_INTR_STATUS_ALL_BITS);
    InterruptDis(LCDC_IRQ);
    rtos_time_delay_ms(40);
    LCDC_Cmd(LCDC, DISABLE);
    __DSB();
    rtos_time_delay_ms(5);
}

/* Show one colour: wait ~1 frame, reload, hold for ms. */
static void show(const char *label, uint8_t r, uint8_t g, uint8_t b, uint32_t ms)
{
    fb_fill(r, g, b);
    rtos_time_delay_ms(20);   /* ~1 frame at 60 Hz before shadow reload */
    lcdc_reload();
    printf("[display_lcdc] %s\n", label);
    rtos_time_delay_ms(ms);
}

/* ---- Test task ---- */

static void display_lcdc_task(void *param)
{
    SemaphoreHandle_t done = (SemaphoreHandle_t)param;

    printf("[display_lcdc] fb_addr = 0x%08X\n", (unsigned)s_fb_addr);

    /* Backlight OFF until panel is up */
    GPIO_InitTypeDef g = {0};
    g.GPIO_Pin  = PIN_BACKLIGHT;
    g.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_Init(&g);
    GPIO_WriteBit(PIN_BACKLIGHT, 0);

    st7701p_hw_init();
    lcdc_pinmux();
    printf("[display_lcdc] rgb_init 480x480 RGB888\n");
    lcdc_rgb_init();

    GPIO_WriteBit(PIN_BACKLIGHT, 1);
    printf("[display_lcdc] backlight ON\n");

    /* Three-colour flush */
    show("solid red",   255,   0,   0, 2000);
    show("solid green",   0, 255,   0, 2000);
    show("solid blue",    0,   0, 255, 2000);

    uint32_t hs = 0, vs = 0;
    LCDC_RGBGetSyncStatus(LCDC, &hs, &vs);
    printf("[display_lcdc] sync hs=%u vs=%u\n", (unsigned)hs, (unsigned)vs);

    lcdc_teardown();
    GPIO_WriteBit(PIN_BACKLIGHT, 0);

    if (s_fb_addr) {
        rtos_mem_free((void *)s_fb_addr);
        s_fb_addr = 0;
    }

    printf("[display_lcdc] success\n");

    /* Drain ARM write buffer before the context switch induced by
     * rtos_task_delete(), so no buffered peripheral write faults. */
    __DSB();
    __ISB();
    xSemaphoreGive(done);
    rtos_time_delay_ms(10);
    rtos_task_delete(NULL);
}

/* ---- Public entry (called from AT+CLAW=display_lcdc) ---- */
void display_lcdc_run(void)
{
    s_fb_addr = fb_alloc();
    if (!s_fb_addr) {
        printf("[display_lcdc] fb alloc failed (need %d bytes)\n", PANEL_FB_BYTES);
        return;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[display_lcdc] semaphore create failed\n");
        rtos_mem_free((void *)s_fb_addr);
        s_fb_addr = 0;
        return;
    }

    if (rtos_task_create(NULL, "display_lcdc_task", display_lcdc_task, done,
                         4096, 1) != RTK_SUCCESS) {
        printf("[display_lcdc] task create failed\n");
        vSemaphoreDelete(done);
        rtos_mem_free((void *)s_fb_addr);
        s_fb_addr = 0;
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}
