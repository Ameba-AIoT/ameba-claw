/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * panel_spi_st7789.c — TX-only SPI transport for the LVGL ST7789 chip driver.
 *
 * See design_spec/display/phase1_panel_hal.md §2 for the rationale.  Key points
 * this file implements (and the traps it avoids):
 *   - SSI is configured TMOD_TO (transmit-only): no RX FIFO, no RX DMA, no RX
 *     drain.  ST7789 has no MISO.
 *   - CMD path (st7789_panel_send_cmd) is CPU polling direct-write; never DMA.
 *   - COLOR path (st7789_panel_send_color) arms a single TX-DMA over LVGL's contiguous
 *     px_map, blocks on a completion sema (given by the TX-DMA callback — the
 *     slave-style completion path, NOT the master RX-done path), then polls
 *     SSI_Busy() until the shifter drains before raising CS, and finally calls
 *     lv_display_flush_ready().
 *   - CS is a manual GPIO held low for a whole (cmd|data) transaction.
 */

#include "panel_spi_st7789.h"

#include <string.h>

#include "ameba_soc.h"
#include "ameba_gdma.h"
#include "os_wrapper.h"

#include "ameba_claw_defs.h"

/* ---- file-static panel context (single owner, single panel) -------------- */

typedef struct {
    panel_cfg_t       cfg;
    SPI_TypeDef      *dev;
    rtos_sema_t       dma_done;      /* given by TX-DMA completion callback     */
    GDMA_InitTypeDef  dma_tx;        /* re-armed per color transfer             */
    uint8_t           ready;         /* 1 after a successful st7789_panel_init         */
} panel_ctx_t;

static panel_ctx_t s_panel;

/* ---- small GPIO helpers -------------------------------------------------- */

static void gpio_out_init(uint16_t pin, int level)
{
    GPIO_InitTypeDef init;
    init.GPIO_Pin  = (u32)pin;
    init.GPIO_Mode = GPIO_Mode_OUT;
    init.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(&init);
    GPIO_WriteBit((u32)pin, level ? 1 : 0);
}

static inline void cs_low(void)   { GPIO_WriteBit((u32)s_panel.cfg.cs_pin, 0); }
static inline void cs_high(void)  { GPIO_WriteBit((u32)s_panel.cfg.cs_pin, 1); }
static inline void dc_cmd(void)   { GPIO_WriteBit((u32)s_panel.cfg.dc_pin, 0); }
static inline void dc_data(void)  { GPIO_WriteBit((u32)s_panel.cfg.dc_pin, 1); }

/* Bounded wait for the SSI bus to go idle (BUSY bit clear).  A HW fault must
 * not hang the whole task, so cap the spin and bail. */
static void wait_bus_idle(SPI_TypeDef *dev)
{
    uint32_t guard = 0;
    while (SSI_Busy(dev)) {
        if (++guard > CLAW_DISPLAY_SPI_POLL_GUARD) {
            break;
        }
    }
}

/* ---- CPU polling byte writer (command + small params) -------------------- */

static void spi_write_cpu(const uint8_t *buf, size_t len)
{
    SPI_TypeDef *dev = s_panel.dev;
    for (size_t i = 0; i < len; i++) {
        /* TMOD_TO: TX FIFO only, no mirrored RX byte to drain. */
        uint32_t guard = 0;
        while (!SSI_Writeable(dev)) {
            if (++guard > CLAW_DISPLAY_SPI_POLL_GUARD) {
                return; /* FIFO wedged — bail rather than hang forever */
            }
        }
        SSI_WriteData(dev, (u32)buf[i]);
    }
}

/* ---- TX-DMA completion callback (slave-style: give sema directly) -------- */

static u32 panel_dma_tx_cb(void *param)
{
    (void)param;
    SPI_TypeDef *dev = s_panel.dev;

    GDMA_ClearINT(s_panel.dma_tx.GDMA_Index, s_panel.dma_tx.GDMA_ChNum);
    GDMA_Cmd(s_panel.dma_tx.GDMA_Index, s_panel.dma_tx.GDMA_ChNum, DISABLE);
    GDMA_ChnlFree(s_panel.dma_tx.GDMA_Index, s_panel.dma_tx.GDMA_ChNum);
    SSI_SetDmaEnable(dev, DISABLE, SPI_BIT_TDMAE);
    SSI_SlaveErrRecovery(dev);

    rtos_sema_give(s_panel.dma_done);
    return 0;
}

/* ---- shared single-block TX-DMA (arm → block on completion → teardown) ---- */

/*
 * Arm one TX-only DMA over a contiguous block and block until the shifter has
 * drained (via the completion sema).  On timeout the channel is torn down so it
 * is not leaked.  SSI_TXGDMA_Init does its own DCache_CleanInvalidate over the
 * whole Length (ameba_spi.c), so the source data is made coherent here — no
 * separate DCache maintenance is required (an extra clean would be redundant;
 * see phase2_present_fastpath.md §2.3).  Caller owns CS / DC / RAMWR framing.
 */
static void dma_tx_block(SPI_TypeDef *dev, uint8_t *param, size_t param_size)
{
    /* Drain any stale completion signal, then arm TX-only DMA. */
    while (rtos_sema_take(s_panel.dma_done, 0) == RTK_SUCCESS) { }

    SSI_TXGDMA_Init((u32)s_panel.cfg.spi_idx, &s_panel.dma_tx, NULL,
                    (IRQ_FUN)panel_dma_tx_cb, param, (u32)param_size);
    SSI_SetDmaEnable(dev, ENABLE, SPI_BIT_TDMAE);

    if (rtos_sema_take(s_panel.dma_done, CLAW_DISPLAY_DMA_TIMEOUT_MS) != RTK_SUCCESS) {
        /* Timeout: tear down the channel so we don't leak it. */
        GDMA_Cmd(s_panel.dma_tx.GDMA_Index, s_panel.dma_tx.GDMA_ChNum, DISABLE);
        GDMA_ClearINT(s_panel.dma_tx.GDMA_Index, s_panel.dma_tx.GDMA_ChNum);
        GDMA_ChnlFree(s_panel.dma_tx.GDMA_Index, s_panel.dma_tx.GDMA_ChNum);
        SSI_SetDmaEnable(dev, DISABLE, SPI_BIT_TDMAE);
    }
}

/* ---- LVGL callback: send one command + its params (CPU, never DMA) ------- */

void st7789_panel_send_cmd(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                    const uint8_t *param, size_t param_size)
{
    (void)disp;
    cs_low();
    dc_cmd();
    spi_write_cpu(cmd, cmd_size);
    if (param_size) {
        dc_data();
        spi_write_cpu(param, param_size);
    }
    wait_bus_idle(s_panel.dev);   /* bus idle before releasing CS */
    cs_high();
}

/* ---- LVGL callback: send one contiguous pixel block via TX-DMA ----------- */

void st7789_panel_send_color(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                      uint8_t *param, size_t param_size)
{
    SPI_TypeDef *dev = s_panel.dev;

    if (param_size == 0) {
        if (disp) {
            lv_display_flush_ready(disp);
        }
        return;
    }

    cs_low();
    dc_cmd();
    spi_write_cpu(cmd, cmd_size);   /* RAMWR (0x2C) + any wrap continuation */
    wait_bus_idle(dev);        /* command must be on the wire before data */
    dc_data();

    /* px_map is contiguous (LVGL partial buffer); arm a single TX-DMA over it. */
    dma_tx_block(dev, param, param_size);

    /* TX DMA done != on-screen: FIFO + shifter still draining. Wait bus idle. */
    wait_bus_idle(dev);
    cs_high();

    if (disp) {
        lv_display_flush_ready(disp);
    }
}

/* ---- fast path: push a contiguous block into a window in one framing + DMA - */

/*
 * st7789_panel_present_window — bypass LVGL's partial flush and push a ready
 * contiguous RGB565_SWAPPED block into the window (x,y,w,h) in a single
 * CASET/RASET/RAMWR + one TX-DMA.  See phase2_present_fastpath.md §2 / §5.
 *
 * CASET/RASET reuse the proven st7789_panel_send_cmd path verbatim (each a
 * self-contained CS pulse); only the RAMWR + data segment is hand-rolled,
 * mirroring st7789_panel_send_color's DMA segment exactly.
 *
 * Window coordinates are absolute panel coords — correct for the current
 * 240x240, MADCTL=0x00, no-gap panel.  NOTE: if a gap/offset or rotation is ever
 * added (lv_st7789_set_gap / set_rotation), these coordinates would need the
 * same gap the LVGL flush path applies — this fast path does not consult
 * LVGL's x_gap/y_gap.
 */
void st7789_panel_present_window(uint8_t *fb, uint16_t x, uint16_t y,
                                 uint16_t w, uint16_t h)
{
    SPI_TypeDef *dev = s_panel.dev;

    if (!fb || w == 0 || h == 0) {
        return;
    }

    uint16_t xs = x, xe = (uint16_t)(x + w - 1);
    uint16_t ys = y, ye = (uint16_t)(y + h - 1);
    uint8_t  caset[4] = { (uint8_t)(xs >> 8), (uint8_t)(xs & 0xFF),
                          (uint8_t)(xe >> 8), (uint8_t)(xe & 0xFF) };
    uint8_t  raset[4] = { (uint8_t)(ys >> 8), (uint8_t)(ys & 0xFF),
                          (uint8_t)(ye >> 8), (uint8_t)(ye & 0xFF) };
    uint8_t  cmd;

    /* Column / page address windows via the proven CPU cmd path. */
    cmd = 0x2A; st7789_panel_send_cmd(NULL, &cmd, 1, caset, 4);   /* CASET */
    cmd = 0x2B; st7789_panel_send_cmd(NULL, &cmd, 1, raset, 4);   /* RASET */

    /* RAMWR + one TX-DMA over the block (mirrors send_color's data leg). */
    cmd = 0x2C;
    cs_low();
    dc_cmd();
    spi_write_cpu(&cmd, 1);         /* RAMWR (0x2C) */
    wait_bus_idle(dev);             /* command on the wire before switching DC */
    dc_data();

    dma_tx_block(dev, fb, (size_t)w * (size_t)h * 2u);

    /* TX DMA done != on-screen: FIFO + shifter still draining. Wait bus idle. */
    wait_bus_idle(dev);
    cs_high();
}

void st7789_panel_present_full(uint8_t *fb, uint16_t w, uint16_t h)
{
    st7789_panel_present_window(fb, 0, 0, w, h);
}

/* ---- bring-up ------------------------------------------------------------ */

int st7789_panel_init(const panel_cfg_t *cfg)
{
    if (!cfg) {
        return -1;
    }

    memset(&s_panel, 0, sizeof(s_panel));
    s_panel.cfg = *cfg;
    s_panel.dev = SPI_DEV_TABLE[cfg->spi_idx].SPIx;

    if (rtos_sema_create(&s_panel.dma_done, 0, 1) != RTK_SUCCESS) {
        return -2;
    }

    /* SSI clock + role BEFORE SSI_Init. */
    if (cfg->spi_idx == 1) {
        RCC_PeriphClockCmd(APBPeriph_SPI1, APBPeriph_SPI1_CLOCK, ENABLE);
    } else {
        RCC_PeriphClockCmd(APBPeriph_SPI0, APBPeriph_SPI0_CLOCK, ENABLE);
    }
    SSI_SetRole(s_panel.dev, SSI_MASTER);

    /* CLK / MOSI use the SPI pinmux group; CS is a manual GPIO (see phase1 §2). */
    Pinmux_Config((u8)cfg->clk_pin,
                  cfg->spi_idx == 1 ? PINMUX_FUNCTION_SPI1 : PINMUX_FUNCTION_SPI0);
    Pinmux_Config((u8)cfg->mosi_pin,
                  cfg->spi_idx == 1 ? PINMUX_FUNCTION_SPI1 : PINMUX_FUNCTION_SPI0);

    SSI_InitTypeDef ssi;
    SSI_StructInit(&ssi);
    ssi.SPI_Role          = SSI_MASTER;
    ssi.SPI_DataFrameSize = DFS_8_BITS;
    ssi.SPI_SclkPolarity  = SCPOL_INACTIVE_IS_LOW;   /* ST7789 SPI mode 0 */
    ssi.SPI_SclkPhase     = SCPH_TOGGLES_IN_MIDDLE;
    ssi.SPI_ClockDivider  = CLAW_DISPLAY_SPI_CLKDIV;
    ssi.SPI_TransferMode  = TMOD_TO;                 /* transmit-only, no RX */
    SSI_Init(s_panel.dev, &ssi);
    SSI_Cmd(s_panel.dev, ENABLE);

    /* Control GPIOs: DC/CS/RST/BLK as outputs. */
    gpio_out_init(cfg->dc_pin,  1);
    gpio_out_init(cfg->cs_pin,  1);   /* idle high */
    gpio_out_init(cfg->rst_pin, 1);
    gpio_out_init(cfg->blk_pin, 0);   /* backlight off until panel is up */

    /* Hard reset: low ≥10ms, then high, wait ≥120ms before commands. */
    GPIO_WriteBit((u32)cfg->rst_pin, 0);
    rtos_time_delay_ms(CLAW_DISPLAY_RESET_LOW_MS);
    GPIO_WriteBit((u32)cfg->rst_pin, 1);
    rtos_time_delay_ms(CLAW_DISPLAY_RESET_HIGH_MS);

    s_panel.ready = 1;
    st7789_panel_backlight(1);
    return 0;
}

void st7789_panel_backlight(int on)
{
    if (s_panel.ready) {
        GPIO_WriteBit((u32)s_panel.cfg.blk_pin, on ? 1 : 0);
    }
}

void st7789_panel_deinit(void)
{
    if (!s_panel.ready) {
        return;
    }
    st7789_panel_backlight(0);
    if (s_panel.dma_done) {
        rtos_sema_delete(s_panel.dma_done);
        s_panel.dma_done = NULL;
    }
    s_panel.ready = 0;
}
