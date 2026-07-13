/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * lua_lcdc_test_provision.c — Provision and run the LCDC RGB test script.
 *
 * Script is NOT auto-run on boot.
 * Trigger: AT+CLAW=lcdc,rgb,st7262
 *
 * The test sets up st7262-specific pinmux and backlight/display-on GPIOs,
 * initialises the 800×480 RGB panel, fills the screen with solid colours
 * and colour bars, then prints "success".
 *
 * All board-specific parameters (pin numbers, timing values) live here in
 * the test script, not in the driver (lua_driver_lcdc.c).
 *
 * Pin mapping (RTL8721F eval board + st7262 800×480 panel):
 *   D0–D23 : PB_15,PB_17,PB_21,PB_18,PA_6,PA_8,PA_7,PA_10,
 *             PB_9,PB_11,PB_10,PB_16,PB_22,PB_23,PB_14,PB_12,
 *             PA_22,PA_25,PA_29,PB_4,PB_5,PB_6,PB_7,PB_8
 *   HSYNC  : PA_16   VSYNC : PA_13   DCLK : PA_9   DE : PA_14
 *   BLEN   : PB_3  (GPIO, active high — backlight enable)
 *   DISP   : PA_17 (GPIO, active high — panel display-on)
 */

#include <stdio.h>
#include <string.h>

/* When defined, the sRGB test displays the image from test_lcdc_srgb_fig.h
 * instead of the geometric patterns. */
/* #define LCDC_SRGB_SHOW_IMAGE */


#include "ameba_soc.h"
#include "ameba_lcdc.h"

#ifdef LCDC_SRGB_SHOW_IMAGE
#include "test_lcdc_srgb_fig.h"   /* const u8 pic320240_24bpp[320*240*3] */
#endif

#include "os_wrapper.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* ---- ST7701P SPI init ---- */
#include "spi_api.h"

/* ---- GT911 I2C polling ---- */
#include "i2c_api.h"
#include "i2c_ex_api.h"

/* ---- Framebuffer allocation helpers (malloc-based, test-only) ----
 * Use rtos_mem_malloc so the allocator controls placement and lifetime.
 * Caller must free the returned pointer after the LCDC DMA is stopped. */

/* st7262 RGB565 framebuffer */
#define ST7262_FB_WIDTH        800
#define ST7262_FB_HEIGHT       480
#define ST7262_FB_BYTES_PER_PX 2   /* RGB565 */

static uint32_t lcdc_test_alloc_fb_st7262(void)
{
    uint32_t fb_size = ST7262_FB_WIDTH * ST7262_FB_HEIGHT * ST7262_FB_BYTES_PER_PX;
    void *buf = rtos_mem_malloc(fb_size);
    if (!buf) { return 0; }
    memset(buf, 0, fb_size);
    DCache_Clean((uint32_t)buf, fb_size);
    return (uint32_t)buf;
}

/* ILI9806 RGB888 framebuffer (480×800, 3 bytes/pixel = 1,152,000 bytes) */
#define ILI9806_FB_WIDTH        480
#define ILI9806_FB_HEIGHT       800
#define ILI9806_FB_BYTES_PER_PX 3   /* RGB888 */

static uint32_t lcdc_test_alloc_fb_ili9806(void)
{
    uint32_t fb_size = ILI9806_FB_WIDTH * ILI9806_FB_HEIGHT * ILI9806_FB_BYTES_PER_PX;
    void *buf = rtos_mem_malloc(fb_size);
    if (!buf) { return 0; }
    memset(buf, 0, fb_size);
    DCache_Clean((uint32_t)buf, fb_size);
    return (uint32_t)buf;
}

/* ST7272A RGB888 framebuffer (320×240, 3 bytes/pixel = 230,400 bytes) */
#define ST7272A_FB_WIDTH        320
#define ST7272A_FB_HEIGHT       240
#define ST7272A_FB_BYTES_PER_PX 3   /* RGB888 */

static uint32_t lcdc_test_alloc_fb_st7272a(void)
{
    uint32_t fb_size = ST7272A_FB_WIDTH * ST7272A_FB_HEIGHT * ST7272A_FB_BYTES_PER_PX;
    void *buf = rtos_mem_malloc(fb_size);
    if (!buf) { return 0; }
    memset(buf, 0, fb_size);
    DCache_Clean((uint32_t)buf, fb_size);
    return (uint32_t)buf;
}

/* ST7701P RGB888 framebuffer (480×480, 3 bytes/pixel = 691,200 bytes) */
#define ST7701P_FB_WIDTH        480
#define ST7701P_FB_HEIGHT       480
#define ST7701P_FB_BYTES_PER_PX 3   /* RGB888 */

static uint32_t lcdc_test_alloc_fb_st7701p(void)
{
    uint32_t fb_size = ST7701P_FB_WIDTH * ST7701P_FB_HEIGHT * ST7701P_FB_BYTES_PER_PX;
    void *buf = rtos_mem_malloc(fb_size);
    if (!buf) { return 0; }
    memset(buf, 0, fb_size);
    DCache_Clean((uint32_t)buf, fb_size);
    return (uint32_t)buf;
}

/* ---- ST7701P SPI + GPIO init -------------------------------------------- */
/* The ST7701P needs a one-shot 9-bit SPI register config sequence before the
 * RGB interface is started.  We do it here in C (not Lua) because the SPI
 * frame width is 9 bits, which the Lua SPI driver does not support.          */

static spi_t s_st7701p_spi;

static void _st7701p_spi_cmd(uint16_t cmd)
{
    spi_master_write(&s_st7701p_spi, (int)cmd);
}

static void _st7701p_spi_dat(uint16_t data)
{
    spi_master_write(&s_st7701p_spi, (int)(data | 0x100u));
}

/* Alias expected by panel_st7701p_rgb_spi.inc */
#define spi_write_command(sp, cmd)  _st7701p_spi_cmd((uint16_t)(cmd))
#define spi_write_data(sp, dat)     _st7701p_spi_dat((uint16_t)(dat))

static void st7701p_send_init_cmds(void)
{
    /* ST7701P register initialization sequence.
     * Ported from panel_st7701p_rgb_spi.inc — no component/ui/ dependency. */
    _st7701p_spi_cmd(0xFF); _st7701p_spi_dat(0x77); _st7701p_spi_dat(0x01);
    _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x13);
    _st7701p_spi_cmd(0xEF); _st7701p_spi_dat(0x08);
    _st7701p_spi_cmd(0xFF); _st7701p_spi_dat(0x77); _st7701p_spi_dat(0x01);
    _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x10);
    _st7701p_spi_cmd(0xC0); _st7701p_spi_dat(0x3B); _st7701p_spi_dat(0x00);
    _st7701p_spi_cmd(0xC1); _st7701p_spi_dat(0x0D); _st7701p_spi_dat(0x02);
    _st7701p_spi_cmd(0xC2); _st7701p_spi_dat(0x37); _st7701p_spi_dat(0x08);
    _st7701p_spi_cmd(0xC7); _st7701p_spi_dat(0x00);
    _st7701p_spi_cmd(0xCC); _st7701p_spi_dat(0x18);
    _st7701p_spi_cmd(0xB0);
    _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x11); _st7701p_spi_dat(0x17);
    _st7701p_spi_dat(0x0E); _st7701p_spi_dat(0x12); _st7701p_spi_dat(0x06);
    _st7701p_spi_dat(0x06); _st7701p_spi_dat(0x08); _st7701p_spi_dat(0x08);
    _st7701p_spi_dat(0x20); _st7701p_spi_dat(0x04); _st7701p_spi_dat(0x11);
    _st7701p_spi_dat(0x0F); _st7701p_spi_dat(0x29); _st7701p_spi_dat(0x30);
    _st7701p_spi_dat(0x1F);
    _st7701p_spi_cmd(0xB1);
    _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x13); _st7701p_spi_dat(0x18);
    _st7701p_spi_dat(0x0F); _st7701p_spi_dat(0x12); _st7701p_spi_dat(0x07);
    _st7701p_spi_dat(0x06); _st7701p_spi_dat(0x08); _st7701p_spi_dat(0x07);
    _st7701p_spi_dat(0x21); _st7701p_spi_dat(0x04); _st7701p_spi_dat(0x12);
    _st7701p_spi_dat(0x10); _st7701p_spi_dat(0x29); _st7701p_spi_dat(0x34);
    _st7701p_spi_dat(0x1F);
    _st7701p_spi_cmd(0xFF); _st7701p_spi_dat(0x77); _st7701p_spi_dat(0x01);
    _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x11);
    _st7701p_spi_cmd(0xB0); _st7701p_spi_dat(0x60);
    _st7701p_spi_cmd(0xB1); _st7701p_spi_dat(0x32);
    _st7701p_spi_cmd(0xB2); _st7701p_spi_dat(0x8A);
    _st7701p_spi_cmd(0xB3); _st7701p_spi_dat(0x80);
    _st7701p_spi_cmd(0xB5); _st7701p_spi_dat(0x4B);
    _st7701p_spi_cmd(0xB7); _st7701p_spi_dat(0x85);
    _st7701p_spi_cmd(0xB8); _st7701p_spi_dat(0x21);
    _st7701p_spi_cmd(0xC0); _st7701p_spi_dat(0x07);
    _st7701p_spi_cmd(0xC1); _st7701p_spi_dat(0x78);
    _st7701p_spi_cmd(0xC2); _st7701p_spi_dat(0x78);
    _st7701p_spi_cmd(0xE0); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x1B); _st7701p_spi_dat(0x02);
    _st7701p_spi_cmd(0xE1);
    _st7701p_spi_dat(0x08); _st7701p_spi_dat(0xA0); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00);
    _st7701p_spi_dat(0x07); _st7701p_spi_dat(0xA0); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00);
    _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x44); _st7701p_spi_dat(0x44);
    _st7701p_spi_cmd(0xE2);
    _st7701p_spi_dat(0x11); _st7701p_spi_dat(0x11); _st7701p_spi_dat(0x44); _st7701p_spi_dat(0x44);
    _st7701p_spi_dat(0xED); _st7701p_spi_dat(0xA0); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00);
    _st7701p_spi_dat(0xEC); _st7701p_spi_dat(0xA0); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00);
    _st7701p_spi_cmd(0xE3);
    _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x11); _st7701p_spi_dat(0x11);
    _st7701p_spi_cmd(0xE4); _st7701p_spi_dat(0x44); _st7701p_spi_dat(0x44);
    _st7701p_spi_cmd(0xE5);
    _st7701p_spi_dat(0x0A); _st7701p_spi_dat(0xE9); _st7701p_spi_dat(0xD8); _st7701p_spi_dat(0xA0);
    _st7701p_spi_dat(0x0C); _st7701p_spi_dat(0xEB); _st7701p_spi_dat(0xD8); _st7701p_spi_dat(0xA0);
    _st7701p_spi_dat(0x0E); _st7701p_spi_dat(0xED); _st7701p_spi_dat(0xD8); _st7701p_spi_dat(0xA0);
    _st7701p_spi_dat(0x10); _st7701p_spi_dat(0xEF); _st7701p_spi_dat(0xD8); _st7701p_spi_dat(0xA0);
    _st7701p_spi_cmd(0xE6);
    _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x11); _st7701p_spi_dat(0x11);
    _st7701p_spi_cmd(0xE7); _st7701p_spi_dat(0x44); _st7701p_spi_dat(0x44);
    _st7701p_spi_cmd(0xE8);
    _st7701p_spi_dat(0x09); _st7701p_spi_dat(0xE8); _st7701p_spi_dat(0xD8); _st7701p_spi_dat(0xA0);
    _st7701p_spi_dat(0x0B); _st7701p_spi_dat(0xEA); _st7701p_spi_dat(0xD8); _st7701p_spi_dat(0xA0);
    _st7701p_spi_dat(0x0D); _st7701p_spi_dat(0xEC); _st7701p_spi_dat(0xD8); _st7701p_spi_dat(0xA0);
    _st7701p_spi_dat(0x0F); _st7701p_spi_dat(0xEE); _st7701p_spi_dat(0xD8); _st7701p_spi_dat(0xA0);
    _st7701p_spi_cmd(0xEB);
    _st7701p_spi_dat(0x02); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0xE4); _st7701p_spi_dat(0xE4);
    _st7701p_spi_dat(0x88); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x40);
    _st7701p_spi_cmd(0xEC); _st7701p_spi_dat(0x3C); _st7701p_spi_dat(0x00);
    _st7701p_spi_cmd(0xED);
    _st7701p_spi_dat(0xAB); _st7701p_spi_dat(0x89); _st7701p_spi_dat(0x76); _st7701p_spi_dat(0x54);
    _st7701p_spi_dat(0x02); _st7701p_spi_dat(0xFF); _st7701p_spi_dat(0xFF); _st7701p_spi_dat(0xFF);
    _st7701p_spi_dat(0xFF); _st7701p_spi_dat(0xFF); _st7701p_spi_dat(0xFF); _st7701p_spi_dat(0x20);
    _st7701p_spi_dat(0x45); _st7701p_spi_dat(0x67); _st7701p_spi_dat(0x98); _st7701p_spi_dat(0xBA);
    _st7701p_spi_cmd(0xEF);
    _st7701p_spi_dat(0x08); _st7701p_spi_dat(0x08); _st7701p_spi_dat(0x08);
    _st7701p_spi_dat(0x45); _st7701p_spi_dat(0x3F); _st7701p_spi_dat(0x54);
    _st7701p_spi_cmd(0xFF); _st7701p_spi_dat(0x77); _st7701p_spi_dat(0x01);
    _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x13);
    _st7701p_spi_cmd(0xE8); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x0E);
    _st7701p_spi_cmd(0xFF); _st7701p_spi_dat(0x77); _st7701p_spi_dat(0x01);
    _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00);
    _st7701p_spi_cmd(0x11);
    rtos_time_delay_ms(120);
    _st7701p_spi_cmd(0xFF); _st7701p_spi_dat(0x77); _st7701p_spi_dat(0x01);
    _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x13);
    _st7701p_spi_cmd(0xE8); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x0C);
    rtos_time_delay_ms(120);
    _st7701p_spi_cmd(0xE8); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00);
    _st7701p_spi_cmd(0xFF); _st7701p_spi_dat(0x77); _st7701p_spi_dat(0x01);
    _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00); _st7701p_spi_dat(0x00);
    _st7701p_spi_cmd(0x29);
    _st7701p_spi_cmd(0x36); _st7701p_spi_dat(0x00);
}

#undef spi_write_command
#undef spi_write_data

static void st7701p_hw_init(void)
{
    /* --- Panel RESET: PA_23 --- */
    GPIO_InitTypeDef g = {0};
    g.GPIO_Pin  = _PA_23;
    g.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_Init(&g);

    GPIO_WriteBit(_PA_23, 1);
    rtos_time_delay_ms(4);
    GPIO_WriteBit(_PA_23, 0);
    rtos_time_delay_ms(30);
    GPIO_WriteBit(_PA_23, 1);
    rtos_time_delay_ms(120);

    /* --- SPI1: CS=PA_20, SCLK=PA_21, MOSI=PA_22, 9-bit Mode3 5MHz --- */
    s_st7701p_spi.spi_idx = MBED_SPI1;
    spi_init(&s_st7701p_spi, _PA_22, 0xFFFFFFFF, _PA_21, _PA_20);
    spi_frequency(&s_st7701p_spi, 5000000);
    spi_format(&s_st7701p_spi, 9, 3, 0);

    /* --- Send ST7701P register init sequence --- */
    st7701p_send_init_cmds();
}

/* ---- GT911 I2C polling --------------------------------------------------- */
/* Minimal polling implementation — no IRQ, no work-queue.
 * Coordinates are transformed to match the panel orientation
 * (INVERSE_X=1, INVERSE_Y=1 as in input_touch_gt911.c).                     */

#define GT911_I2C_ADDR   0x14
#define GT911_SDA        _PA_29
#define GT911_SCL        _PA_30
#define GT911_RST        _PB_0
#define GT911_INT        _PA_31
#define GT911_XSIZE      480
#define GT911_YSIZE      480

#define GT911_REG_GSTID  0x814E   /* touch status register */
#define GT911_REG_POINT  0x814F   /* first touch-point data */

static i2c_t s_gt911_i2c;

static int gt911_reg_write(uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val};
    return i2c_write(&s_gt911_i2c, GT911_I2C_ADDR, (char *)buf, 3, 2);
}

static int gt911_reg_read(uint16_t reg, uint8_t *out, int len)
{
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    if (i2c_write(&s_gt911_i2c, GT911_I2C_ADDR, (char *)addr, 2, 1) != 2) {
        return -1;
    }
    return i2c_read(&s_gt911_i2c, GT911_I2C_ADDR, (char *)out, len, 1);
}

static void gt911_hw_init(void)
{
    /* RST and INT as output for address-selection reset sequence */
    GPIO_InitTypeDef g = {0};
    g.GPIO_Mode = GPIO_Mode_OUT;

    g.GPIO_Pin = GT911_RST;
    GPIO_Init(&g);
    g.GPIO_Pin = GT911_INT;
    GPIO_Init(&g);

    /* Reset sequence (selects I2C address 0x14) */
    GPIO_WriteBit(GT911_INT, 0);
    GPIO_WriteBit(GT911_RST, 0);
    rtos_time_delay_ms(10);
    GPIO_WriteBit(GT911_INT, 1);
    DelayUs(100);
    GPIO_WriteBit(GT911_RST, 1);
    rtos_time_delay_ms(5);
    GPIO_WriteBit(GT911_INT, 0);
    rtos_time_delay_ms(50);

    /* INT as floating input */
    g.GPIO_Pin  = GT911_INT;
    g.GPIO_Mode = GPIO_Mode_IN;
    g.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(&g);

    /* I2C0 init at 400 kHz */
    s_gt911_i2c.i2c_idx = 0;
    i2c_init(&s_gt911_i2c, GT911_SDA, GT911_SCL);
    i2c_frequency(&s_gt911_i2c, 400000);
    i2c_restart_disable(&s_gt911_i2c);
    rtos_time_delay_ms(100);

    /* Clear any pending status */
    gt911_reg_write(GT911_REG_GSTID, 0x00);
}

typedef struct {
    uint16_t x;
    uint16_t y;
    bool     pressed;
    bool     valid;
} gt911_point_t;

static gt911_point_t gt911_poll_hw(void)
{
    gt911_point_t pt = {0, 0, false, false};
    uint8_t status = 0;

    if (gt911_reg_read(GT911_REG_GSTID, &status, 1) < 0) {
        return pt;
    }
    if (!(status & 0x80)) {
        return pt;   /* data not ready */
    }

    uint8_t num = status & 0x0F;

    /* Always clear the ready flag */
    gt911_reg_write(GT911_REG_GSTID, 0x00);

    if (num == 0) {
        pt.valid   = true;
        pt.pressed = false;
        return pt;
    }

    /* Read 7 bytes from 0x814F: track_id x_lo x_hi y_lo y_hi sz_lo sz_hi */
    uint8_t data[7] = {0};
    if (gt911_reg_read(GT911_REG_POINT, data, 7) < 0) {
        return pt;
    }

    uint16_t raw_x = (uint16_t)(data[1] | ((uint16_t)data[2] << 8));
    uint16_t raw_y = (uint16_t)(data[3] | ((uint16_t)data[4] << 8));

    /* Apply same transform as input_touch_gt911.c (INVERSE_X=1, INVERSE_Y=1) */
    pt.x       = (raw_x < GT911_XSIZE) ? (GT911_XSIZE - raw_x) : 0;
    pt.y       = (raw_y < GT911_YSIZE) ? (GT911_YSIZE - raw_y) : 0;
    pt.pressed = true;
    pt.valid   = true;
    return pt;
}

/* Lua binding: gt911_poll() → nil | {x=n, y=n, pressed=bool} */
static int lua_gt911_poll(lua_State *L)
{
    gt911_point_t pt = gt911_poll_hw();
    if (!pt.valid) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)pt.x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, (lua_Integer)pt.y);
    lua_setfield(L, -2, "y");
    lua_pushboolean(L, pt.pressed ? 1 : 0);
    lua_setfield(L, -2, "pressed");
    return 1;
}

/* ---- Test-only framebuffer helpers (fill_color / fill_rect / set_pixel) ----
 * These belong in the test layer, not in the driver.
 * They are registered as plain Lua globals before each test script runs.
 * s_test_fb is set by lua_lcdc_run() before creating the task. */

static struct {
    uint32_t addr;
    uint32_t width;
    uint32_t height;
    int      bpp;   /* 2=RGB565, 3=RGB888, 4=ARGB8888 */
} s_test_fb;

/* fill_color(r, g, b) */
static int lcdc_test_fill_color(lua_State *L)
{
    uint8_t r = (uint8_t)luaL_checkinteger(L, 1);
    uint8_t g = (uint8_t)luaL_checkinteger(L, 2);
    uint8_t b = (uint8_t)luaL_checkinteger(L, 3);
    uint32_t n = s_test_fb.width * s_test_fb.height;
    if (s_test_fb.bpp == 4) {
        uint32_t argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        uint32_t *p = (uint32_t *)s_test_fb.addr;
        for (uint32_t i = 0; i < n; i++) { p[i] = argb; }
        DCache_Clean(s_test_fb.addr, n * 4);
    } else if (s_test_fb.bpp == 3) {
        uint8_t *p = (uint8_t *)s_test_fb.addr;
        for (uint32_t i = 0; i < n; i++) { p[i*3]=r; p[i*3+1]=g; p[i*3+2]=b; }
        DCache_Clean(s_test_fb.addr, n * 3);
    } else {
        uint16_t px = (uint16_t)(((uint32_t)(r&0xF8)<<8)|((uint32_t)(g&0xFC)<<3)|(b>>3));
        uint16_t *p = (uint16_t *)s_test_fb.addr;
        for (uint32_t i = 0; i < n; i++) { p[i] = px; }
        DCache_Clean(s_test_fb.addr, n * 2);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* fill_rect(x, y, w, h, r, g, b) */
static int lcdc_test_fill_rect(lua_State *L)
{
    uint32_t x = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t y = (uint32_t)luaL_checkinteger(L, 2);
    uint32_t w = (uint32_t)luaL_checkinteger(L, 3);
    uint32_t h = (uint32_t)luaL_checkinteger(L, 4);
    uint8_t  r = (uint8_t)luaL_checkinteger(L, 5);
    uint8_t  g = (uint8_t)luaL_checkinteger(L, 6);
    uint8_t  b = (uint8_t)luaL_checkinteger(L, 7);
    uint32_t x2 = x + w; if (x2 > s_test_fb.width)  x2 = s_test_fb.width;
    uint32_t y2 = y + h; if (y2 > s_test_fb.height) y2 = s_test_fb.height;
    uint32_t cols = x2 - x;
    if (s_test_fb.bpp == 4) {
        uint32_t argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        for (uint32_t row = y; row < y2; row++) {
            uint32_t *line = (uint32_t *)s_test_fb.addr + row * s_test_fb.width + x;
            for (uint32_t c = 0; c < cols; c++) { line[c] = argb; }
            DCache_Clean((uint32_t)line, cols * 4);
        }
    } else if (s_test_fb.bpp == 3) {
        for (uint32_t row = y; row < y2; row++) {
            uint8_t *line = (uint8_t *)s_test_fb.addr + (row * s_test_fb.width + x) * 3;
            for (uint32_t c = 0; c < cols; c++) { line[c*3]=r; line[c*3+1]=g; line[c*3+2]=b; }
            DCache_Clean((uint32_t)line, cols * 3);
        }
    } else {
        uint16_t px = (uint16_t)(((uint32_t)(r&0xF8)<<8)|((uint32_t)(g&0xFC)<<3)|(b>>3));
        for (uint32_t row = y; row < y2; row++) {
            uint16_t *line = (uint16_t *)s_test_fb.addr + row * s_test_fb.width + x;
            for (uint32_t c = 0; c < cols; c++) { line[c] = px; }
            DCache_Clean((uint32_t)line, cols * 2);
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* set_pixel(x, y, r, g, b) */
static int lcdc_test_set_pixel(lua_State *L)
{
    uint32_t x = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t y = (uint32_t)luaL_checkinteger(L, 2);
    uint8_t  r = (uint8_t)luaL_checkinteger(L, 3);
    uint8_t  g = (uint8_t)luaL_checkinteger(L, 4);
    uint8_t  b = (uint8_t)luaL_checkinteger(L, 5);
    if (x >= s_test_fb.width || y >= s_test_fb.height) {
        return luaL_error(L, "set_pixel: coord (%d,%d) out of range", (int)x, (int)y);
    }
    uint32_t idx = y * s_test_fb.width + x;
    if (s_test_fb.bpp == 4) {
        uint32_t argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        ((uint32_t *)s_test_fb.addr)[idx] = argb;
        DCache_Clean(s_test_fb.addr + idx * 4, 4);
    } else if (s_test_fb.bpp == 3) {
        uint8_t *p = (uint8_t *)s_test_fb.addr + idx * 3;
        p[0]=r; p[1]=g; p[2]=b;
        DCache_Clean((uint32_t)p, 3);
    } else {
        uint16_t px = (uint16_t)(((uint32_t)(r&0xF8)<<8)|((uint32_t)(g&0xFC)<<3)|(b>>3));
        ((uint16_t *)s_test_fb.addr)[idx] = px;
        DCache_Clean(s_test_fb.addr + idx * 2, 2);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* Scripts are auto-generated from the .lua source files at configure time.
 * Edit the .lua source files; do NOT edit the generated headers directly. */
#include "lcdc_rgb_st7262_lua.h"
#include "lcdc_mcu_ili9806_lua.h"
#include "lcdc_srgb_st7272a_lua.h"
#include "lcdc_rgb_st7701p_touch_gt711_lua.h"


/* ---- ST7272A image display script (used when LCDC_SRGB_SHOW_IMAGE is defined) ----
 * The C layer copies pic320240_24bpp[] into the framebuffer before running this
 * script, so the script only needs to init the panel and call lcdc.update(). */
#ifdef LCDC_SRGB_SHOW_IMAGE
static const char s_lcdc_srgb_image_script[] =
    "local lcdc=require('lcdc') local gpio=require('gpio') local sys=require('sys')\n"
    "local fb_addr=_lcdc_fb_addr\n"
    "local PIN_BLEN=0x23 local PIN_RESET=0x36\n"
    "print(string.format('[lcdc srgb img] fb_addr=0x%08X',fb_addr))\n"
    "gpio.set_direction(PIN_RESET,'output') gpio.set_level(PIN_RESET,1) sys.sleep_ms(100)\n"
    "gpio.set_direction(PIN_BLEN,'output')  gpio.set_level(PIN_BLEN,1)\n"
    "lcdc.pinmux({d0=0x06,d1=0x08,d2=0x07,d3=0x0A,d4=0x0B,d5=0x09,d6=0x11,d7=0x10,\n"
    "    hsync=0x35,vsync=0x31,dclk=0x2E,de=0x0E})\n"
    "print('[lcdc srgb img] init (8-bit, 320x240, 35Hz)')\n"
    "local ok,err=pcall(function()\n"
    "    if not lcdc.rgb_init({fb_addr=fb_addr,width=320,height=240,\n"
    "        if_width='8bit',input_fmt='rgb888',output_fmt='bgr888',\n"
    "        vsw=4,vbp=8,vfp=8,hsw=4,hbp=39,hfp=8,refresh_freq=35,\n"
    "        en_pol=1,hs_pol=0,vs_pol=0,dclk_edge=1}) then error('rgb_init failed') end\n"
    "end)\n"
    "if not ok then print('[lcdc srgb img] init err:'..tostring(err))\n"
    "    gpio.set_level(PIN_BLEN,0) return end\n"
    "lcdc.update()\n"
    "print('[lcdc srgb img] displaying image')\n"
    "sys.sleep_ms(3000)\n"
    "local sync=lcdc.rgb_get_sync_status()\n"
    "print(string.format('[lcdc srgb img] sync hs=%d vs=%d',sync.hs,sync.vs))\n"
    "lcdc.deinit() gpio.set_level(PIN_BLEN,0)\n"
    "print('success')\n";
#endif /* LCDC_SRGB_SHOW_IMAGE */


/* ---- Provision: write scripts to VFS ---- */

static void inject_gt911_globals(lua_State *L)
{
    lua_pushcfunction(L, lua_gt911_poll);
    lua_setglobal(L, "gt911_poll");
}

void lua_driver_lcdc_provision(void)
{
    const struct { const char *path; const char *src; } scripts[] = {
        { "vfs:test_lcdc_rgb_st7262.lua",               s_lcdc_rgb_st7262_script               },
        { "vfs:test_lcdc_mcu_ili9806.lua",              s_lcdc_mcu_ili9806_script              },
        { "vfs:test_lcdc_srgb_st7272a.lua",             s_lcdc_srgb_st7272a_script             },
        { "vfs:test_lcdc_rgb_st7701p_touch_gt711.lua",  s_lcdc_rgb_st7701p_touch_gt711_script  },
    };
    for (int i = 0; i < (int)(sizeof(scripts) / sizeof(scripts[0])); i++) {
        FILE *f = fopen(scripts[i].path, "w");
        if (f) {
            fwrite(scripts[i].src, 1, strlen(scripts[i].src), f);
            fclose(f);
        }
    }
}

/* ---- On-demand execution ---- */

typedef struct {
    const char       *script;
    uint32_t          fb_addr;
    SemaphoreHandle_t done;
    void            (*inject_extras)(lua_State *L);   /* optional extra globals */
} lcdc_task_arg_t;

static void lcdc_lua_task(void *param)
{
    lcdc_task_arg_t *arg = (lcdc_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[lcdc] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        lua_pushinteger(L, (lua_Integer)arg->fb_addr);
        lua_setglobal(L, "_lcdc_fb_addr");
        /* Inject test-only pixel helpers as plain Lua globals */
        lua_pushcfunction(L, lcdc_test_fill_color); lua_setglobal(L, "fill_color");
        lua_pushcfunction(L, lcdc_test_fill_rect);  lua_setglobal(L, "fill_rect");
        lua_pushcfunction(L, lcdc_test_set_pixel);  lua_setglobal(L, "set_pixel");
        if (arg->inject_extras) {
            arg->inject_extras(L);
        }
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[lcdc] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[lcdc] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    /* Free framebuffer AFTER lua_close (Lua state closed) and AFTER
     * lcdc.deinit() was called inside the script (DMA already stopped). */
    if (arg->fb_addr) {
        rtos_mem_free((void *)arg->fb_addr);
    }

    /* Drain ARM write buffer before triggering FreeRTOS context switch.
     * Without this, any buffered peripheral write (LCDC/I2C/SPI) that
     * completed asynchronously causes an imprecise bus fault during the
     * PendSV context switch that rtos_task_delete() induces. */
    __DSB();
    __ISB();
    xSemaphoreGive(arg->done);
    rtos_time_delay_ms(10);   /* let main thread run before this task exits */
    rtos_task_delete(NULL);
}

void lua_lcdc_run(const char *if_mode, const char *panel)
{
    const char *script   = NULL;
    uint32_t    fb_addr  = 0;

    if (strcmp(if_mode, "rgb") == 0 && strcmp(panel, "st7262") == 0) {
        script  = s_lcdc_rgb_st7262_script;
        fb_addr = lcdc_test_alloc_fb_st7262();
        s_test_fb.width = ST7262_FB_WIDTH; s_test_fb.height = ST7262_FB_HEIGHT;
        s_test_fb.bpp   = ST7262_FB_BYTES_PER_PX;
    } else if (strcmp(if_mode, "mcu") == 0 && strcmp(panel, "ili9806") == 0) {
        script  = s_lcdc_mcu_ili9806_script;
        fb_addr = lcdc_test_alloc_fb_ili9806();
        s_test_fb.width = ILI9806_FB_WIDTH; s_test_fb.height = ILI9806_FB_HEIGHT;
        s_test_fb.bpp   = ILI9806_FB_BYTES_PER_PX;
    } else if (strcmp(if_mode, "srgb") == 0 && strcmp(panel, "st7272a") == 0) {
        fb_addr = lcdc_test_alloc_fb_st7272a();
        s_test_fb.width = ST7272A_FB_WIDTH; s_test_fb.height = ST7272A_FB_HEIGHT;
        s_test_fb.bpp   = ST7272A_FB_BYTES_PER_PX;
#ifdef LCDC_SRGB_SHOW_IMAGE
        /* Copy image into framebuffer, then run the simple display script. */
        memcpy((void *)fb_addr, pic320240_24bpp, ST7272A_FB_WIDTH * ST7272A_FB_HEIGHT * ST7272A_FB_BYTES_PER_PX);
        DCache_Clean(fb_addr, ST7272A_FB_WIDTH * ST7272A_FB_HEIGHT * ST7272A_FB_BYTES_PER_PX);
        script  = s_lcdc_srgb_image_script;
#else
        script  = s_lcdc_srgb_st7272a_script;
#endif
    } else if (strcmp(if_mode, "rgb") == 0 && strcmp(panel, "st7701p") == 0) {
        st7701p_hw_init();
        gt911_hw_init();
        script  = s_lcdc_rgb_st7701p_touch_gt711_script;
        fb_addr = lcdc_test_alloc_fb_st7701p();
        if (!fb_addr) {
            printf("[lcdc] st7701p fb alloc failed (need %u bytes)\n",
                   ST7701P_FB_WIDTH * ST7701P_FB_HEIGHT * ST7701P_FB_BYTES_PER_PX);
            return;
        }
        s_test_fb.width = ST7701P_FB_WIDTH;
        s_test_fb.height = ST7701P_FB_HEIGHT;
        s_test_fb.bpp   = ST7701P_FB_BYTES_PER_PX;
    } else {
        printf("[lcdc] unsupported: if=%s panel=%s "
               "(supported: rgb,st7262 | rgb,st7701p | mcu,ili9806 | srgb,st7272a)\n",
               if_mode, panel);
        return;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[lcdc] semaphore create failed\n");
        return;
    }

    s_test_fb.addr = fb_addr;
    void (*extras)(lua_State *L) = NULL;
    if (strcmp(if_mode, "rgb") == 0 && strcmp(panel, "st7701p") == 0) {
        extras = inject_gt911_globals;
    }
    lcdc_task_arg_t arg = {
        .script        = script,
        .fb_addr       = fb_addr,
        .done          = done,
        .inject_extras = extras,
    };

    /* 20 KB stack: pixel helpers run in C; Lua overhead is small */
    if (rtos_task_create(NULL, "lcdc_lua_task", lcdc_lua_task, &arg,
                         20480, 1) != RTK_SUCCESS) {
        printf("[lcdc] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}
