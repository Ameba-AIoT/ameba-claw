/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_driver_audio.c — Lua audio driver for Ameba RTOS (RTL8721F).
**
** Provides require("audio") with input (DMIC) and output (I2S speaker) APIs
** backed directly by the SPORT0 peripheral + internal CODEC ADC.
** No dependency on component/audio; all paths go through fwlib SOC drivers.
**
** INPUT API  (DMIC via SPORT0 RX + CODEC ADC):
**   local h, err = audio.new_input(sr, ch, bps [, adc_vol [, dmic_clk_pin, dmic_data_pin]])
**   audio.close(h)
**   local ok, err = audio.record_wav(h, path, duration_ms)
**   audio.set_gain(h, vol)           -- ADC digital volume 0x00..0x7F
**   audio.set_mute(h, bool)
**   local rms = audio.mic_read_level(h)
**   local s1,t1,s2,t2,pass = audio.snr_thd(h)  -- SNR & THD in dB for each channel
**
** OUTPUT API  (I2S speaker via SPORT0 TX, e.g. MAX98357A):
**   local h, err = audio.new_output(sr, ch, bps [, bclk_pin, ws_pin, dout_pin])
**   audio.play_tone(h, freq_hz, vol)   -- start sine at freq_hz, non-blocking
**   audio.play_wav(h, path [, vol])    -- play WAV file, blocking
**   audio.close(h)                     -- works for both input and output handles
**
** SPORT0 is shared: DMIC and speaker can coexist in full-duplex.
** The driver tracks which directions are active to skip AUDIO_SP_Reset when
** the other direction is already running.
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"

#include "ameba_soc.h"
#include "ameba_audio.h"
#include "ameba_sport.h"
#include "ameba_audio_clock.h"
#include "ameba_pinmux.h"
#include "ameba_gdma.h"
#include "os_wrapper.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- constants ----------------------------------------------------------- */

#define AUDIO_SPORT_IDX     0
#define AUDIO_CHUNK_SIZE    4096          /* bytes per DMA transfer */
#define AUDIO_DMA_TIMEOUT   2000          /* ms to wait per DMA chunk */
#define AUDIO_META          "ameba.audio"
#define AUDIO_OUT_META      "ameba.audio_out"

/* I2S output pin pinmux functions (from ameba_pinmux.h) */
#define PINMUX_FN_I2S0_BCLK   32
#define PINMUX_FN_I2S0_WS     34
#define PINMUX_FN_I2S0_DIO3   38

/* ---- static DMA buffers (must be non-cached, 32-byte aligned) ------------ */

SRAM_NOCACHE_DATA_SECTION
static u8 s_dma_buf[2][AUDIO_CHUNK_SIZE] __attribute__((aligned(32)));

SRAM_NOCACHE_DATA_SECTION
static u8 s_tx_dma_buf[2][AUDIO_CHUNK_SIZE] __attribute__((aligned(32)));

/* ---- SPORT0 direction-usage flags ----------------------------------------
 * Prevent AUDIO_SP_Reset when the other direction is already active.        */
static volatile u8 s_sport0_rx_active = 0;
static volatile u8 s_sport0_tx_active = 0;

/* ---- RX driver singleton state ------------------------------------------- */

typedef struct {
    GDMA_InitTypeDef gdma;
    rtos_sema_t      sema;
    volatile u8      ping_pong;
    volatile u8      ready_buf;
    volatile u8      stop_flag;
    u8               sema_created;
} audio_drv_t;

static audio_drv_t s_drv;

/* ---- TX driver singleton state ------------------------------------------- */

#define TX_MODE_IDLE    0
#define TX_MODE_TONE    1
#define TX_MODE_FILE    2
#define TX_MODE_STREAM  3   /* raw PCM streaming — sema only, caller drives restart */

typedef struct {
    GDMA_InitTypeDef gdma;
    rtos_sema_t      sema;
    volatile u8      ping_pong;
    volatile u8      ready_buf;
    volatile u8      stop_flag;
    u8               sema_created;
    u8               gdma_active;
    u8               mode;
    float            tone_phase;
    float            tone_phase_inc;
    float            tone_vol;
    u32              sample_rate;
    u8               channels;
} audio_tx_drv_t;

static audio_tx_drv_t s_tx_drv;

/* ---- Lua input userdata -------------------------------------------------- */

typedef struct {
    u32 sample_rate;
    u8  channels;
    u8  bits_per_sample;
    u8  adc_vol;
    u8  open;
    u32 dmic_clk_pin;
    u32 dmic_data_pin;
} audio_lua_t;

/* ---- Lua output userdata ------------------------------------------------- */

typedef struct {
    u32 sample_rate;
    u8  channels;
    u8  bits_per_sample;
    u8  open;
    u32 bclk_pin;
    u32 ws_pin;
    u32 dout_pin;
} audio_out_lua_t;

/* =====================================================================
 * RX (DMIC) path
 * ===================================================================== */

static u32 audio_rx_dma_cb(void *data)
{
    audio_drv_t *drv = (audio_drv_t *)data;

    GDMA_ClearINT(drv->gdma.GDMA_Index, drv->gdma.GDMA_ChNum);

    u8 done = drv->ping_pong;
    drv->ready_buf = done;

    if (!drv->stop_flag) {
        drv->ping_pong ^= 1;
        AUDIO_SP_RXGDMA_Restart(drv->gdma.GDMA_Index, drv->gdma.GDMA_ChNum,
                                 (u32)s_dma_buf[drv->ping_pong], AUDIO_CHUNK_SIZE);
    }

    rtos_sema_give(drv->sema);
    return 0;
}

static u32 hz_to_codec_sr(u32 rate)
{
    if (rate <= 8000)  return SR_8K;
    if (rate <= 16000) return SR_16K;
    if (rate <= 44100) return SR_44P1K;
    return SR_48K;
}

static int audio_hw_open(audio_lua_t *h)
{
    SP_InitTypeDef  sp;
    I2S_InitTypeDef i2s;

    RCC_PeriphClockCmd(APBPeriph_SPORT, APBPeriph_SPORT_CLOCK, ENABLE);
    RCC_PeriphClockSource_SPORT(AUDIO_SPORT0_DEV, CKSL_I2S_XTAL40M);
    RCC_PeriphClockCmd(APBPeriph_AC,    APBPeriph_AC_CLOCK,    ENABLE);

    if (h->dmic_clk_pin)  { Pinmux_Config(h->dmic_clk_pin,  PINMUX_FUNCTION_DMIC_CLK); }
    if (h->dmic_data_pin) { Pinmux_Config(h->dmic_data_pin, PINMUX_FUNCTION_DMIC_DATA); }

    if (!s_sport0_rx_active && !s_sport0_tx_active) {
        AUDIO_SP_Reset(AUDIO_SPORT_IDX);
    }

    AUDIO_SP_StructInit(&sp);
    sp.SP_SelDataFormat    = SP_DF_I2S;
    sp.SP_SelI2SMonoStereo = (h->channels == 1) ? SP_CH_MONO : SP_CH_STEREO;
    sp.SP_SelWordLen       = SP_RXWL_16;
    sp.SP_SelTDM           = SP_RX_NOTDM;
    sp.SP_SelFIFO          = SP_RX_FIFO2;
    sp.SP_SR               = h->sample_rate;
    sp.SP_SelClk           = CKSL_I2S_XTAL40M;
    AUDIO_SP_Init(AUDIO_SPORT_IDX, SP_DIR_RX, &sp);

    s_sport0_rx_active = 1;

    AUDIO_CODEC_I2S_StructInit(&i2s);
    i2s.CODEC_SelI2SRxSR  = hz_to_codec_sr(h->sample_rate);
    i2s.CODEC_SelRxI2STdm = I2S_NOTDM;
    AUDIO_CODEC_Record(I2S0, APP_DMIC_RECORD, &i2s);
    AUDIO_CODEC_SetDmicClk(DMIC_2P5M, ENABLE);
    AUDIO_CODEC_SetADCVolume(ADC1, h->adc_vol);
    AUDIO_CODEC_SetADCVolume(ADC2, h->adc_vol);

    if (!s_drv.sema_created) {
        rtos_sema_create_binary(&s_drv.sema);
        s_drv.sema_created = 1;
    }

    s_drv.stop_flag = 0;
    s_drv.ping_pong = 0;
    s_drv.ready_buf = 0;
    memset(s_dma_buf, 0, sizeof(s_dma_buf));

    AUDIO_SP_DmaCmd(AUDIO_SPORT_IDX, ENABLE);
    AUDIO_SP_RXGDMA_Init(AUDIO_SPORT_IDX, GDMA_INT, &s_drv.gdma, &s_drv,
                         (IRQ_FUN)audio_rx_dma_cb,
                         (u8 *)s_dma_buf[0], AUDIO_CHUNK_SIZE);
    DelayMs(100);
    AUDIO_SP_RXStart(AUDIO_SPORT_IDX, ENABLE);

    return 0;
}

static void audio_hw_close(void)
{
    s_drv.stop_flag = 1;
    AUDIO_SP_RXStart(AUDIO_SPORT_IDX, DISABLE);

    if (!s_sport0_tx_active) {
        AUDIO_SP_DmaCmd(AUDIO_SPORT_IDX, DISABLE);
    }

    GDMA_ClearINT(s_drv.gdma.GDMA_Index, s_drv.gdma.GDMA_ChNum);
    GDMA_Abort(s_drv.gdma.GDMA_Index, s_drv.gdma.GDMA_ChNum);
    GDMA_ChnlFree(s_drv.gdma.GDMA_Index, s_drv.gdma.GDMA_ChNum);
    AUDIO_SP_Deinit(AUDIO_SPORT_IDX, SP_DIR_RX);
    s_sport0_rx_active = 0;
}

/* =====================================================================
 * TX (speaker) path
 * ===================================================================== */

static void audio_fill_tone_chunk(u8 *buf, u32 size, audio_tx_drv_t *drv)
{
    int16_t *p     = (int16_t *)buf;
    u32      nfr   = size / ((u32)drv->channels * 2u);
    float    pi2   = 2.0f * 3.14159265358979f;

    for (u32 i = 0; i < nfr; i++) {
        int16_t s = (int16_t)(32767.0f * drv->tone_vol * sinf(drv->tone_phase));
        for (u8 c = 0; c < drv->channels; c++) {
            p[i * drv->channels + c] = s;
        }
        drv->tone_phase += drv->tone_phase_inc;
        if (drv->tone_phase >= pi2) {
            drv->tone_phase -= pi2;
        }
    }
    DCache_CleanInvalidate((u32)buf, size);
}

static u32 audio_tx_dma_cb(void *data)
{
    audio_tx_drv_t *drv = (audio_tx_drv_t *)data;

    GDMA_ClearINT(drv->gdma.GDMA_Index, drv->gdma.GDMA_ChNum);

    if (drv->stop_flag) {
        return 0;
    }

    u8 done = drv->ping_pong;
    drv->ready_buf = done;

    if (drv->mode == TX_MODE_TONE) {
        /* Restart DMA FIRST with the already-filled other buffer to avoid FIFO underrun,
         * then refill the just-completed buffer for the next round. */
        drv->ping_pong ^= 1;
        AUDIO_SP_TXGDMA_Restart(drv->gdma.GDMA_Index, drv->gdma.GDMA_ChNum,
                                (u32)s_tx_dma_buf[drv->ping_pong], AUDIO_CHUNK_SIZE);
        audio_fill_tone_chunk(s_tx_dma_buf[done], AUDIO_CHUNK_SIZE, drv);
    } else if (drv->mode == TX_MODE_FILE) {
        drv->ping_pong ^= 1;
        AUDIO_SP_TXGDMA_Restart(drv->gdma.GDMA_Index, drv->gdma.GDMA_ChNum,
                                (u32)s_tx_dma_buf[drv->ping_pong], AUDIO_CHUNK_SIZE);
        rtos_sema_give(drv->sema);
    } else if (drv->mode == TX_MODE_STREAM) {
        /* Ping-pong like TX_MODE_FILE: immediately restart DMA with the OTHER
         * buffer (already filled by write_chunk) to prevent FIFO underrun.
         * Then wake the producer to refill the completed buffer. */
        drv->ping_pong ^= 1;
        AUDIO_SP_TXGDMA_Restart(drv->gdma.GDMA_Index, drv->gdma.GDMA_ChNum,
                                (u32)s_tx_dma_buf[drv->ping_pong], AUDIO_CHUNK_SIZE);
        rtos_sema_give(drv->sema);
    }

    return 0;
}

static int audio_hw_tx_open(audio_out_lua_t *h)
{
    RCC_PeriphClockCmd(APBPeriph_SPORT, APBPeriph_SPORT_CLOCK, ENABLE);
    RCC_PeriphClockSource_SPORT(AUDIO_SPORT0_DEV, CKSL_I2S_XTAL40M);

    /* Configure I2S output pins */
    if (h->bclk_pin) { Pinmux_Config(h->bclk_pin, PINMUX_FN_I2S0_BCLK); }
    if (h->ws_pin)   { Pinmux_Config(h->ws_pin,   PINMUX_FN_I2S0_WS); }
    if (h->dout_pin) {
        /* Route SPORT0 DIO3 as DOUT_0 (output) */
        u32 tmp = HAL_READ32(PINMUX_REG_BASE, REG_I2S_CTRL);
        tmp |= PAD_BIT_SP0_DIO3_MUXSEL;
        HAL_WRITE32(PINMUX_REG_BASE, REG_I2S_CTRL, tmp);
        Pinmux_Config(h->dout_pin, PINMUX_FN_I2S0_DIO3);
    }

    if (!s_sport0_rx_active && !s_sport0_tx_active) {
        AUDIO_SP_Reset(AUDIO_SPORT_IDX);
    }

    SP_InitTypeDef sp;
    AUDIO_SP_StructInit(&sp);
    sp.SP_SelDataFormat    = SP_DF_I2S;
    sp.SP_SelI2SMonoStereo = (h->channels == 1) ? SP_CH_MONO : SP_CH_STEREO;
    sp.SP_SelWordLen       = SP_TXWL_16;
    sp.SP_SelChLen         = SP_TXCL_32;
    sp.SP_SelTDM           = SP_TX_NOTDM;
    sp.SP_SelFIFO          = SP_TX_FIFO2;
    sp.SP_SR               = h->sample_rate;
    sp.SP_SelClk           = CKSL_I2S_XTAL40M;
    sp.SP_SetMultiIO       = SP_TX_MULTIIO_DIS;
    AUDIO_SP_Init(AUDIO_SPORT_IDX, SP_DIR_TX, &sp);

    s_sport0_tx_active = 1;

    if (!s_tx_drv.sema_created) {
        rtos_sema_create_binary(&s_tx_drv.sema);
        s_tx_drv.sema_created = 1;
    }

    s_tx_drv.mode       = TX_MODE_IDLE;
    s_tx_drv.gdma_active = 0;
    s_tx_drv.stop_flag  = 0;
    s_tx_drv.ping_pong  = 0;
    s_tx_drv.ready_buf  = 0;
    s_tx_drv.sample_rate = h->sample_rate;
    s_tx_drv.channels    = h->channels;
    memset(s_tx_dma_buf, 0, sizeof(s_tx_dma_buf));
    /* Drain any stale semaphore signal left by a previous audio_sp_close().
     * Without this, write_chunk takes the stale signal immediately and
     * restarts DMA before the current transfer completes — producing silence. */
    while (rtos_sema_take(s_tx_drv.sema, 0) == RTK_SUCCESS) {}

    return 0;
}

static void audio_hw_tx_stop_gdma(void)
{
    if (!s_tx_drv.gdma_active) {
        return;
    }
    s_tx_drv.stop_flag  = 1;
    s_tx_drv.mode       = TX_MODE_IDLE;
    AUDIO_SP_TXStart(AUDIO_SPORT_IDX, DISABLE);
    GDMA_ClearINT(s_tx_drv.gdma.GDMA_Index, s_tx_drv.gdma.GDMA_ChNum);
    GDMA_Abort(s_tx_drv.gdma.GDMA_Index, s_tx_drv.gdma.GDMA_ChNum);
    GDMA_ChnlFree(s_tx_drv.gdma.GDMA_Index, s_tx_drv.gdma.GDMA_ChNum);
    s_tx_drv.gdma_active = 0;
}

static void audio_hw_tx_close(void)
{
    audio_hw_tx_stop_gdma();
    if (!s_sport0_rx_active) {
        AUDIO_SP_DmaCmd(AUDIO_SPORT_IDX, DISABLE);
    }
    AUDIO_SP_Deinit(AUDIO_SPORT_IDX, SP_DIR_TX);
    s_sport0_tx_active = 0;
}

/* ---- WAV helpers --------------------------------------------------------- */

static void write_le32(u8 *p, u32 v)
{
    p[0] = (u8)(v);
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void write_le16(u8 *p, u16 v)
{
    p[0] = (u8)(v);
    p[1] = (u8)(v >> 8);
}

static u32 read_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u16 read_le16(const u8 *p) {
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static int wav_write_header(FILE *f, u32 sr, u16 ch, u16 bps, u32 data_bytes)
{
    u8  hdr[44];
    u32 byte_rate  = sr * ch * (bps / 8u);
    u16 blk_align  = (u16)(ch * (bps / 8u));
    u32 riff_size  = 36u + data_bytes;

    memcpy(hdr + 0,  "RIFF", 4);
    write_le32(hdr + 4,  riff_size);
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    write_le32(hdr + 16, 16u);
    write_le16(hdr + 20, 1u);
    write_le16(hdr + 22, ch);
    write_le32(hdr + 24, sr);
    write_le32(hdr + 28, byte_rate);
    write_le16(hdr + 32, blk_align);
    write_le16(hdr + 34, bps);
    memcpy(hdr + 36, "data", 4);
    write_le32(hdr + 40, data_bytes);

    return (fwrite(hdr, 1, 44, f) == 44) ? 0 : -1;
}

/* Fill a TX DMA buffer from a file, applying volume, returns bytes read. */
static u32 fill_tx_buf_from_file(FILE *f, float vol, u8 *buf, u32 size)
{
    u32 n = (u32)fread(buf, 1, size, f);
    if (n < size) {
        memset(buf + n, 0, size - n);
    }
    if (vol < 0.999f) {
        int16_t *s = (int16_t *)buf;
        u32 ns = size / 2u;
        for (u32 i = 0; i < ns; i++) {
            s[i] = (int16_t)((float)s[i] * vol);
        }
    }
    DCache_CleanInvalidate((u32)buf, size);
    return n;
}

/* ---- userdata helpers ---------------------------------------------------- */

static audio_lua_t *laudio_check(lua_State *L, int arg)
{
    audio_lua_t *h = (audio_lua_t *)luaL_checkudata(L, arg, AUDIO_META);
    if (!h->open) {
        luaL_error(L, "attempt to use a closed audio handle");
    }
    return h;
}

static audio_out_lua_t *laudio_out_check(lua_State *L, int arg)
{
    audio_out_lua_t *h = (audio_out_lua_t *)luaL_checkudata(L, arg, AUDIO_OUT_META);
    if (!h->open) {
        luaL_error(L, "attempt to use a closed audio output handle");
    }
    return h;
}

/* ======================================================================
 * SNR / THD calculation (512-point radix-2 FFT)
 * ====================================================================== */

#define FFT_N           512
#define FFT_HARMONIC    6
#define SNR_THRESH_DB   20.0f
#define THD_THRESH_DB   20.0f

static float s_fft_re[FFT_N];
static float s_fft_im[FFT_N];
static float s_fft_mag[FFT_N / 2];
static float s_fft_sq[FFT_N / 2];
static float s_fft_sq_snr[FFT_N / 2];

static void fft_radix2(float *re, float *im)
{
    for (int i = 1, j = 0; i < FFT_N; i++) {
        int bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for (int len = 2; len <= FFT_N; len <<= 1) {
        float ang = -2.0f * 3.14159265358979f / (float)len;
        float wre = cosf(ang);
        float wim = sinf(ang);
        for (int i = 0; i < FFT_N; i += len) {
            float cr = 1.0f, ci = 0.0f;
            int half = len >> 1;
            for (int j = 0; j < half; j++) {
                float ur = re[i + j];
                float ui = im[i + j];
                float vr = re[i + j + half] * cr - im[i + j + half] * ci;
                float vi = re[i + j + half] * ci + im[i + j + half] * cr;
                re[i + j]        = ur + vr;
                im[i + j]        = ui + vi;
                re[i + j + half] = ur - vr;
                im[i + j + half] = ui - vi;
                float ncr = cr * wre - ci * wim;
                ci = cr * wim + ci * wre;
                cr = ncr;
            }
        }
    }
}

static void calc_snr_thd_ch(const float *samples, float *snr_out, float *thd_out, u32 *peak_bin_out)
{
    for (int i = 0; i < FFT_N; i++) {
        float w = 0.5f * (1.0f - cosf(2.0f * 3.14159265358979f * i / (FFT_N - 1)));
        s_fft_re[i] = samples[i] * w;
        s_fft_im[i] = 0.0f;
    }

    fft_radix2(s_fft_re, s_fft_im);

    for (int i = 0; i < FFT_N / 2; i++) {
        s_fft_mag[i] = sqrtf(s_fft_re[i] * s_fft_re[i] + s_fft_im[i] * s_fft_im[i]);
    }

    s_fft_mag[0] = 0.0f;
    s_fft_mag[1] = 0.0f;
    s_fft_mag[2] = 0.0f;

    u32 peak = 3;
    for (u32 i = 4; i < FFT_N / 2; i++) {
        if (s_fft_mag[i] > s_fft_mag[peak]) {
            peak = i;
        }
    }

    for (u32 i = 0; i < FFT_N / 2; i++) {
        s_fft_sq[i] = s_fft_mag[i] * s_fft_mag[i];
    }

    memcpy(s_fft_sq_snr, s_fft_sq, sizeof(s_fft_sq));
    for (u32 h = 2; h <= FFT_HARMONIC; h++) {
        u32 hi = h * peak;
        if (hi < FFT_N / 2) {
            if (hi > 0)                 s_fft_sq_snr[hi - 1] = 0.0f;
            s_fft_sq_snr[hi]            = 0.0f;
            if (hi + 1 < FFT_N / 2)    s_fft_sq_snr[hi + 1] = 0.0f;
        }
    }

    float sig = 0.0f, noise = 0.0f;
    for (u32 i = 0; i < FFT_N / 2; i++) {
        if (i >= peak - 1 && i <= peak + 1) {
            sig   += s_fft_sq_snr[i];
        } else {
            noise += s_fft_sq_snr[i];
        }
    }
    *snr_out = 20.0f * log10f(sqrtf(sig + 1e-12f) / sqrtf(noise + 1e-12f));

    sig = 0.0f; noise = 0.0f;
    for (u32 i = 0; i < FFT_N / 2; i++) {
        if (i >= peak - 1 && i <= peak + 1) {
            sig   += s_fft_sq[i];
        } else {
            noise += s_fft_sq[i];
        }
    }
    *thd_out     = 20.0f * log10f(sqrtf(sig + 1e-12f) / sqrtf(noise + 1e-12f));
    *peak_bin_out = peak;
}

/* ======================================================================
 * Lua INPUT API
 * ====================================================================== */

static int laudio_new_input(lua_State *L)
{
    u32 sr       = (u32)luaL_checkinteger(L, 1);
    u8  ch       = (u8)luaL_checkinteger(L, 2);
    u8  bps      = (u8)luaL_checkinteger(L, 3);
    u8  vol      = (u8)luaL_optinteger(L, 4, 0x2F);
    u32 clk_pin  = (u32)luaL_optinteger(L, 5, 0);
    u32 data_pin = (u32)luaL_optinteger(L, 6, 0);

    if (bps != 16) {
        lua_pushnil(L);
        lua_pushstring(L, "audio: only bits_per_sample=16 supported");
        return 2;
    }
    if (ch != 1 && ch != 2) {
        lua_pushnil(L);
        lua_pushstring(L, "audio: channels must be 1 or 2");
        return 2;
    }

    audio_lua_t *h = (audio_lua_t *)lua_newuserdatauv(L, sizeof(audio_lua_t), 0);
    memset(h, 0, sizeof(*h));
    h->sample_rate     = sr;
    h->channels        = ch;
    h->bits_per_sample = bps;
    h->adc_vol         = vol;
    h->dmic_clk_pin    = clk_pin;
    h->dmic_data_pin   = data_pin;
    h->open            = 1;

    luaL_getmetatable(L, AUDIO_META);
    lua_setmetatable(L, -2);

    if (audio_hw_open(h) != 0) {
        h->open = 0;
        lua_pushnil(L);
        lua_pushstring(L, "audio.new_input: hardware init failed");
        return 2;
    }
    return 1;
}

static int laudio_set_gain(lua_State *L)
{
    audio_lua_t *h   = laudio_check(L, 1);
    u8           vol = (u8)luaL_checkinteger(L, 2);

    h->adc_vol = vol;
    AUDIO_CODEC_SetADCVolume(ADC1, vol);
    AUDIO_CODEC_SetADCVolume(ADC2, vol);
    lua_pushboolean(L, 1);
    return 1;
}

static int laudio_set_mute(lua_State *L)
{
    laudio_check(L, 1);
    int mute  = lua_toboolean(L, 2);
    u32 state = mute ? MUTE : UNMUTE;

    AUDIO_CODEC_SetADCMute(ADC1, state);
    AUDIO_CODEC_SetADCMute(ADC2, state);
    lua_pushboolean(L, 1);
    return 1;
}

static int laudio_record_wav(lua_State *L)
{
    audio_lua_t *h           = laudio_check(L, 1);
    const char  *path        = luaL_checkstring(L, 2);
    u32          duration_ms = (u32)luaL_checkinteger(L, 3);

    u32 bytes_per_sec = h->sample_rate * h->channels * (h->bits_per_sample / 8u);
    u32 total_bytes   = (u32)((u64)bytes_per_sec * duration_ms / 1000u);

    FILE *f = fopen(path, "wb");
    if (!f) {
        lua_pushnil(L);
        lua_pushfstring(L, "audio record_wav: cannot open %s", path);
        return 2;
    }

    if (wav_write_header(f, h->sample_rate, h->channels,
                         h->bits_per_sample, total_bytes) != 0) {
        fclose(f);
        lua_pushnil(L);
        lua_pushstring(L, "audio record_wav: failed to write WAV header");
        return 2;
    }

    u32 recorded = 0;
    int ok = 1;
    while (recorded < total_bytes) {
        if (rtos_sema_take(s_drv.sema, AUDIO_DMA_TIMEOUT) != RTK_SUCCESS) {
            ok = 0;
            break;
        }
        u8  rb       = s_drv.ready_buf;
        u32 to_write = total_bytes - recorded;
        if (to_write > AUDIO_CHUNK_SIZE) {
            to_write = AUDIO_CHUNK_SIZE;
        }
        DCache_Invalidate((u32)s_dma_buf[rb], AUDIO_CHUNK_SIZE);
        if (fwrite(s_dma_buf[rb], 1, to_write, f) != to_write) {
            ok = 0;
            break;
        }
        recorded += to_write;
    }

    fclose(f);

    if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, "audio record_wav: DMA timeout or write error");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int laudio_mic_read_level(lua_State *L)
{
    laudio_check(L, 1);

    double rms = 0.0;
    if (rtos_sema_take(s_drv.sema, AUDIO_DMA_TIMEOUT) == RTK_SUCCESS) {
        u8       rb  = s_drv.ready_buf;
        u32      n   = AUDIO_CHUNK_SIZE / 2u;
        int16_t *p   = (int16_t *)s_dma_buf[rb];
        double   sum = 0.0;

        DCache_Invalidate((u32)s_dma_buf[rb], AUDIO_CHUNK_SIZE);
        for (u32 i = 0; i < n; i++) {
            double s = (double)p[i];
            sum += s * s;
        }
        rms = (n > 0) ? sqrt(sum / (double)n) : 0.0;
    }

    lua_pushnumber(L, (lua_Number)rms);
    return 1;
}

static int laudio_snr_thd(lua_State *L)
{
    audio_lua_t *h = laudio_check(L, 1);

    if (rtos_sema_take(s_drv.sema, AUDIO_DMA_TIMEOUT) != RTK_SUCCESS) {
        lua_pushnil(L);
        lua_pushstring(L, "audio snr_thd: DMA timeout");
        return 2;
    }

    u8 rb = s_drv.ready_buf;
    DCache_Invalidate((u32)s_dma_buf[rb], AUDIO_CHUNK_SIZE);

    static float s_ch1[FFT_N];
    static float s_ch2[FFT_N];

    const u32 SKIP = 64;

    if (h->channels == 2) {
        u32        nwords = AUDIO_CHUNK_SIZE / 4;
        u32        avail  = nwords - SKIP;
        u32        fill   = (avail < FFT_N) ? avail : FFT_N;
        const u32 *w      = (const u32 *)s_dma_buf[rb];
        for (u32 i = 0; i < fill; i++) {
            u32 word = w[SKIP + i];
            s_ch1[i] = (float)(int16_t)(word & 0xFFFF);
            s_ch2[i] = (float)(int16_t)(word >> 16);
        }
        for (u32 i = fill; i < FFT_N; i++) {
            s_ch1[i] = 0.0f;
            s_ch2[i] = 0.0f;
        }
    } else {
        u32           nsamples = AUDIO_CHUNK_SIZE / 2;
        u32           avail    = nsamples - SKIP;
        u32           fill     = (avail < FFT_N) ? avail : FFT_N;
        const int16_t *s       = (const int16_t *)s_dma_buf[rb];
        for (u32 i = 0; i < fill; i++) {
            s_ch1[i] = (float)s[SKIP + i];
            s_ch2[i] = 0.0f;
        }
        for (u32 i = fill; i < FFT_N; i++) {
            s_ch1[i] = 0.0f;
            s_ch2[i] = 0.0f;
        }
    }

    float snr1, thd1, snr2 = 0.0f, thd2 = 0.0f;
    u32   peak1 = 0, peak2 = 0;
    calc_snr_thd_ch(s_ch1, &snr1, &thd1, &peak1);
    if (h->channels == 2) {
        calc_snr_thd_ch(s_ch2, &snr2, &thd2, &peak2);
    }

    int pass = (snr1 >= SNR_THRESH_DB && thd1 >= THD_THRESH_DB);
    if (h->channels == 2) {
        pass = pass && (snr2 >= SNR_THRESH_DB && thd2 >= THD_THRESH_DB);
    }

    /* peak_freq = peak_bin * sample_rate / FFT_N */
    float peak_freq1 = (float)peak1 * (float)h->sample_rate / (float)FFT_N;

    lua_pushnumber(L,  (lua_Number)snr1);
    lua_pushnumber(L,  (lua_Number)thd1);
    lua_pushnumber(L,  (lua_Number)snr2);
    lua_pushnumber(L,  (lua_Number)thd2);
    lua_pushboolean(L, pass);
    lua_pushnumber(L,  (lua_Number)peak_freq1);  /* 6th: dominant frequency Hz */
    return 6;
}

/* ======================================================================
 * Lua OUTPUT API
 * ====================================================================== */

/* audio.new_output(sr, ch, bps [, bclk_pin, ws_pin, dout_pin]) */
static int laudio_new_output(lua_State *L)
{
    u32 sr       = (u32)luaL_checkinteger(L, 1);
    u8  ch       = (u8)luaL_checkinteger(L, 2);
    u8  bps      = (u8)luaL_checkinteger(L, 3);
    u32 bclk_pin = (u32)luaL_optinteger(L, 4, 0);
    u32 ws_pin   = (u32)luaL_optinteger(L, 5, 0);
    u32 dout_pin = (u32)luaL_optinteger(L, 6, 0);

    if (bps != 16) {
        lua_pushnil(L);
        lua_pushstring(L, "audio: only bits_per_sample=16 supported");
        return 2;
    }
    if (ch != 1 && ch != 2) {
        lua_pushnil(L);
        lua_pushstring(L, "audio: channels must be 1 or 2");
        return 2;
    }

    audio_out_lua_t *h = (audio_out_lua_t *)lua_newuserdatauv(L, sizeof(audio_out_lua_t), 0);
    memset(h, 0, sizeof(*h));
    h->sample_rate     = sr;
    h->channels        = ch;
    h->bits_per_sample = bps;
    h->bclk_pin        = bclk_pin;
    h->ws_pin          = ws_pin;
    h->dout_pin        = dout_pin;
    h->open            = 1;

    luaL_getmetatable(L, AUDIO_OUT_META);
    lua_setmetatable(L, -2);

    if (audio_hw_tx_open(h) != 0) {
        h->open = 0;
        lua_pushnil(L);
        lua_pushstring(L, "audio.new_output: hardware init failed");
        return 2;
    }
    return 1;
}

/* audio.play_tone(h, freq_hz, vol)  — non-blocking, starts DMA tone loop */
static int laudio_play_tone(lua_State *L)
{
    audio_out_lua_t *h     = laudio_out_check(L, 1);
    float            freq  = (float)luaL_checknumber(L, 2);
    float            vol   = (float)luaL_optnumber(L, 3, 1.0);

    if (vol > 1.0f) vol = 1.0f;
    if (vol < 0.0f) vol = 0.0f;

    /* Stop any previously active DMA before starting a new tone. */
    audio_hw_tx_stop_gdma();

    s_tx_drv.mode           = TX_MODE_TONE;
    s_tx_drv.stop_flag      = 0;
    s_tx_drv.ping_pong      = 0;
    s_tx_drv.tone_vol       = vol;
    s_tx_drv.tone_phase     = 0.0f;
    s_tx_drv.tone_phase_inc = 2.0f * 3.14159265358979f * freq / (float)h->sample_rate;

    /* Pre-fill both buffers */
    audio_fill_tone_chunk(s_tx_dma_buf[0], AUDIO_CHUNK_SIZE, &s_tx_drv);
    audio_fill_tone_chunk(s_tx_dma_buf[1], AUDIO_CHUNK_SIZE, &s_tx_drv);

    AUDIO_SP_DmaCmd(AUDIO_SPORT_IDX, ENABLE);
    AUDIO_SP_TXGDMA_Init(AUDIO_SPORT_IDX, GDMA_INT, &s_tx_drv.gdma, &s_tx_drv,
                         (IRQ_FUN)audio_tx_dma_cb,
                         s_tx_dma_buf[0], AUDIO_CHUNK_SIZE);
    s_tx_drv.gdma_active = 1;
    AUDIO_SP_TXStart(AUDIO_SPORT_IDX, ENABLE);

    lua_pushboolean(L, 1);
    return 1;
}

/* audio.play_wav(h, path [, vol])  — blocking until file finishes */
static int laudio_play_wav(lua_State *L)
{
    laudio_out_check(L, 1);
    const char      *path = luaL_checkstring(L, 2);
    float            vol  = (float)luaL_optnumber(L, 3, 1.0f);

    if (vol > 1.0f) vol = 1.0f;
    if (vol < 0.0f) vol = 0.0f;

    /* Stop any previously active DMA. */
    audio_hw_tx_stop_gdma();

    FILE *f = fopen(path, "rb");
    if (!f) {
        lua_pushnil(L);
        lua_pushfstring(L, "audio play_wav: cannot open %s", path);
        return 2;
    }

    /* Parse 44-byte PCM WAV header */
    u8 hdr[44];
    if (fread(hdr, 1, 44, f) != 44) {
        fclose(f);
        lua_pushnil(L);
        lua_pushstring(L, "audio play_wav: short WAV header");
        return 2;
    }
    if (memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4) || memcmp(hdr + 12, "fmt ", 4)) {
        fclose(f);
        lua_pushnil(L);
        lua_pushstring(L, "audio play_wav: not a WAV file");
        return 2;
    }
    /* Skip any extra format bytes to reach 'data' chunk (handles fmt chunks > 16 bytes) */
    u32 fmt_size  = read_le32(hdr + 16);
    if (fmt_size > 16) {
        fseek(f, (long)(fmt_size - 16), SEEK_CUR);
    }
    /* Read 'data' chunk header */
    u8 data_hdr[8];
    if (fread(data_hdr, 1, 8, f) != 8) {
        fclose(f);
        lua_pushnil(L);
        lua_pushstring(L, "audio play_wav: missing data chunk");
        return 2;
    }
    u32 data_size = read_le32(data_hdr + 4);
    (void)data_size;

    /* Pre-fill both TX DMA buffers */
    s_tx_drv.mode      = TX_MODE_FILE;
    s_tx_drv.stop_flag = 0;
    s_tx_drv.ping_pong = 0;

    u32 n0 = fill_tx_buf_from_file(f, vol, s_tx_dma_buf[0], AUDIO_CHUNK_SIZE);
    u32 n1 = fill_tx_buf_from_file(f, vol, s_tx_dma_buf[1], AUDIO_CHUNK_SIZE);

    AUDIO_SP_DmaCmd(AUDIO_SPORT_IDX, ENABLE);
    AUDIO_SP_TXGDMA_Init(AUDIO_SPORT_IDX, GDMA_INT, &s_tx_drv.gdma, &s_tx_drv,
                         (IRQ_FUN)audio_tx_dma_cb,
                         s_tx_dma_buf[0], AUDIO_CHUNK_SIZE);
    s_tx_drv.gdma_active = 1;
    AUDIO_SP_TXStart(AUDIO_SPORT_IDX, ENABLE);

    int eof = (n0 < AUDIO_CHUNK_SIZE) || (n1 < AUDIO_CHUNK_SIZE);
    int ok  = 1;

    /* Feed buffers until file exhausted */
    while (!eof) {
        if (rtos_sema_take(s_tx_drv.sema, AUDIO_DMA_TIMEOUT) != RTK_SUCCESS) {
            ok = 0;
            break;
        }
        u8  rb = s_tx_drv.ready_buf;
        u32 n  = fill_tx_buf_from_file(f, vol, s_tx_dma_buf[rb], AUDIO_CHUNK_SIZE);
        if (n < AUDIO_CHUNK_SIZE) {
            eof = 1;
        }
    }

    /* Wait for the last partially-filled buffer to finish playing */
    if (ok && rtos_sema_take(s_tx_drv.sema, AUDIO_DMA_TIMEOUT) != RTK_SUCCESS) {
        ok = 0;
    }

    fclose(f);
    audio_hw_tx_stop_gdma();

    if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, "audio play_wav: DMA timeout");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ======================================================================
 * C-level blocking PCM record  (used by AT+CLAW=rec)
 * ====================================================================== */

int audio_record_pcm_c(const char *path, uint32_t sample_rate,
                       uint8_t channels, uint32_t duration_ms)
{
    audio_lua_t h = {
        .sample_rate     = sample_rate,
        .channels        = channels,
        .bits_per_sample = 16,
        .adc_vol         = 0x2F,
        .open            = 1,
    };

    if (audio_hw_open(&h) != 0) return -1;

    uint32_t bytes_per_ms = sample_rate * channels * 2u / 1000u;
    uint32_t total_bytes  = bytes_per_ms * duration_ms;

    FILE *f = fopen(path, "wb");
    if (!f) { audio_hw_close(); return -1; }

    uint32_t recorded = 0;
    int ok = 1;
    while (recorded < total_bytes) {
        if (rtos_sema_take(s_drv.sema, AUDIO_DMA_TIMEOUT) != RTK_SUCCESS) {
            ok = 0; break;
        }
        uint8_t  rb       = s_drv.ready_buf;
        uint32_t to_write = total_bytes - recorded;
        if (to_write > AUDIO_CHUNK_SIZE) to_write = AUDIO_CHUNK_SIZE;
        DCache_Invalidate((u32)s_dma_buf[rb], AUDIO_CHUNK_SIZE);
        if (fwrite(s_dma_buf[rb], 1, to_write, f) != to_write) { ok = 0; break; }
        recorded += to_write;
    }

    fclose(f);
    audio_hw_close();
    return ok ? 0 : -1;
}

/* ======================================================================
 * C-level DMIC streaming API  (used by cap_audio_stream)
 * ====================================================================== */

int audio_dmic_open(uint32_t sample_rate, uint8_t channels,
                    uint32_t dmic_clk_pin, uint32_t dmic_data_pin)
{
    if (s_drv.sema_created && !s_drv.stop_flag) {
        return 0;  /* already open — idempotent */
    }
    audio_lua_t h = {
        .sample_rate     = sample_rate,
        .channels        = channels,
        .bits_per_sample = 16,
        .adc_vol         = 0x2F,
        .open            = 1,
        .dmic_clk_pin    = dmic_clk_pin,
        .dmic_data_pin   = dmic_data_pin,
    };
    return audio_hw_open(&h);
}

int audio_dmic_read_chunk(const uint8_t **chunk_out, uint32_t timeout_ms)
{
    if (rtos_sema_take(s_drv.sema, timeout_ms) != RTK_SUCCESS) {
        return 0;
    }
    uint8_t rb = s_drv.ready_buf;
    DCache_Invalidate((u32)s_dma_buf[rb], AUDIO_CHUNK_SIZE);
    *chunk_out = s_dma_buf[rb];
    return AUDIO_CHUNK_SIZE;
}

void audio_dmic_close(void)
{
    audio_hw_close();
}

/* ======================================================================
 * C-level speaker (I2S TX) streaming API  (used by cap_audio_stream)
 * Default I2S output pins: PA_10 BCLK, PA_14 LRCLK, PA_16 DATA0
 * ====================================================================== */

/* Default I2S TX pins — must match the physical DAC wiring (e.g. MAX98357A).
 * PA25(0x19)=BCLK  PA26(0x1A)=WS/LRCLK  PA29(0x1D)=DIO3/DOUT */
#define AUDIO_SP_BCLK_PIN   0x19u   /* _PA_25 */
#define AUDIO_SP_LRCLK_PIN  0x1Au   /* _PA_26 */
#define AUDIO_SP_DATA0_PIN  0x1Du   /* _PA_29 */

int audio_sp_open(uint32_t sample_rate, uint8_t channels)
{
    if (s_sport0_tx_active) {
        return 0;  /* already open — idempotent */
    }
    audio_out_lua_t h = {
        .sample_rate     = sample_rate,
        .channels        = channels,
        .bits_per_sample = 16,
        .bclk_pin        = AUDIO_SP_BCLK_PIN,
        .ws_pin          = AUDIO_SP_LRCLK_PIN,
        .dout_pin        = AUDIO_SP_DATA0_PIN,
        .open            = 1,
    };
    return audio_hw_tx_open(&h);
}

int audio_sp_write_chunk(const uint8_t *data, uint32_t len)
{
    if (!s_sport0_tx_active) {
        return -1;
    }
    if (len > AUDIO_CHUNK_SIZE) {
        len = AUDIO_CHUNK_SIZE;
    }

    if (!s_tx_drv.gdma_active) {
        /* First chunk: start DMA in STREAM mode */
        s_tx_drv.mode      = TX_MODE_STREAM;
        s_tx_drv.stop_flag = 0;
        s_tx_drv.ping_pong = 0;
        memcpy(s_tx_dma_buf[0], data, len);
        if (len < AUDIO_CHUNK_SIZE) {
            memset(s_tx_dma_buf[0] + len, 0, AUDIO_CHUNK_SIZE - len);
        }
        /* Pre-fill buf[1] with the same data so the ISR ping-pong restart
         * has valid audio immediately (avoids 256 ms of silence on 2nd DMA). */
        memcpy(s_tx_dma_buf[1], s_tx_dma_buf[0], AUDIO_CHUNK_SIZE);
        DCache_CleanInvalidate((u32)s_tx_dma_buf[0], AUDIO_CHUNK_SIZE);
        DCache_CleanInvalidate((u32)s_tx_dma_buf[1], AUDIO_CHUNK_SIZE);
        AUDIO_SP_DmaCmd(AUDIO_SPORT_IDX, ENABLE);
        AUDIO_SP_TXGDMA_Init(AUDIO_SPORT_IDX, GDMA_INT, &s_tx_drv.gdma, &s_tx_drv,
                             (IRQ_FUN)audio_tx_dma_cb,
                             s_tx_dma_buf[0], AUDIO_CHUNK_SIZE);
        s_tx_drv.gdma_active = 1;
        AUDIO_SP_TXStart(AUDIO_SPORT_IDX, ENABLE);
        return 0;
    }

    /* Ping-pong: wait for ISR to signal (DMA of ping_pong buf done, other buf
     * already restarted). Refill the completed buffer for the next cycle. */
    if (rtos_sema_take(s_tx_drv.sema, AUDIO_DMA_TIMEOUT) != RTK_SUCCESS) {
        return -1;
    }
    if (!s_sport0_tx_active) {
        return -1;
    }

    u8 rb = s_tx_drv.ready_buf;   /* buffer that just finished — safe to overwrite */
    memcpy(s_tx_dma_buf[rb], data, len);
    if (len < AUDIO_CHUNK_SIZE) {
        memset(s_tx_dma_buf[rb] + len, 0, AUDIO_CHUNK_SIZE - len);
    }
    DCache_CleanInvalidate((u32)s_tx_dma_buf[rb], AUDIO_CHUNK_SIZE);
    /* DMA restart is done by ISR (TX_MODE_STREAM ping-pong). */
    return 0;
}

void audio_sp_close(void)
{
    if (!s_sport0_tx_active) {
        return;
    }
    s_tx_drv.stop_flag = 1;
    /* Clear BEFORE sema_give: write_chunk checks this flag after wakeup and must
     * see 0 to avoid calling TXGDMA_Restart concurrently with GDMA_Abort below. */
    s_sport0_tx_active = 0;
    rtos_sema_give(s_tx_drv.sema);  /* unblock any pending write_chunk */
    audio_hw_tx_close();
}

/* ======================================================================
 * Lua streaming wrappers  (for LLM skill authoring)
 * ====================================================================== */

/* audio.start_record([sr [, ch [, clk_pin [, data_pin]]]]) → true | nil, err */
static int laudio_start_record(lua_State *L)
{
    u32 sr       = (u32)luaL_optinteger(L, 1, 8000);
    u8  ch       = (u8)luaL_optinteger(L, 2, 1);
    u32 clk_pin  = (u32)luaL_optinteger(L, 3, 0);
    u32 data_pin = (u32)luaL_optinteger(L, 4, 0);
    if (audio_dmic_open(sr, ch, clk_pin, data_pin) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "audio: start_record failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* audio.read_chunk([timeout_ms]) → string | nil */
static int laudio_read_chunk(lua_State *L)
{
    u32 timeout = (u32)luaL_optinteger(L, 1, 2000);
    const uint8_t *chunk = NULL;
    int n = audio_dmic_read_chunk(&chunk, timeout);
    if (n <= 0 || !chunk) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)chunk, (size_t)n);
    return 1;
}

/* audio.stop_record() → true */
static int laudio_stop_record(lua_State *L)
{
    audio_dmic_close();
    lua_pushboolean(L, 1);
    return 1;
}

/* audio.start_play(sr, ch) → true | nil, err */
static int laudio_start_play(lua_State *L)
{
    u32 sr = (u32)luaL_checkinteger(L, 1);
    u8  ch = (u8)luaL_checkinteger(L, 2);
    if (audio_sp_open(sr, ch) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "audio: start_play failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* audio.write_chunk_play(data) → true | nil, err */
static int laudio_write_chunk_play(lua_State *L)
{
    size_t      len;
    const char *data = luaL_checklstring(L, 1, &len);
    if (audio_sp_write_chunk((const uint8_t *)data, (uint32_t)len) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "audio: write_chunk_play failed (not open or timeout)");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* audio.stop_play() → true */
static int laudio_stop_play(lua_State *L)
{
    audio_sp_close();
    lua_pushboolean(L, 1);
    return 1;
}

/* ======================================================================
 * Shared close / GC
 * ====================================================================== */

static int laudio_close(lua_State *L)
{
    /* Handle input */
    audio_lua_t *h_in = (audio_lua_t *)luaL_testudata(L, 1, AUDIO_META);
    if (h_in) {
        if (h_in->open) {
            audio_hw_close();
            h_in->open = 0;
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    /* Handle output */
    audio_out_lua_t *h_out = (audio_out_lua_t *)luaL_testudata(L, 1, AUDIO_OUT_META);
    if (h_out) {
        if (h_out->open) {
            audio_hw_tx_close();
            h_out->open = 0;
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    luaL_error(L, "audio.close: invalid handle");
    return 0;
}

static int laudio_gc(lua_State *L)
{
    audio_lua_t *h = (audio_lua_t *)luaL_testudata(L, 1, AUDIO_META);
    if (h && h->open) {
        audio_hw_close();
        h->open = 0;
    }
    return 0;
}

static int laudio_out_gc(lua_State *L)
{
    audio_out_lua_t *h = (audio_out_lua_t *)luaL_testudata(L, 1, AUDIO_OUT_META);
    if (h && h->open) {
        audio_hw_tx_close();
        h->open = 0;
    }
    return 0;
}

/* ---- module registration ------------------------------------------------- */

static const luaL_Reg laudio_methods[] = {
    {"close",          laudio_close},
    {"set_gain",       laudio_set_gain},
    {"set_mute",       laudio_set_mute},
    {"record_wav",     laudio_record_wav},
    {"mic_read_level", laudio_mic_read_level},
    {"snr_thd",        laudio_snr_thd},
    {NULL, NULL}
};

static const luaL_Reg laudio_out_methods[] = {
    {"close",          laudio_close},
    {"play_tone",      laudio_play_tone},
    {"play_wav",       laudio_play_wav},
    {NULL, NULL}
};

static const luaL_Reg laudio_funcs[] = {
    {"new_input",         laudio_new_input},
    {"new_output",        laudio_new_output},
    {"close",             laudio_close},
    {"set_gain",          laudio_set_gain},
    {"set_mute",          laudio_set_mute},
    {"record_wav",        laudio_record_wav},
    {"mic_read_level",    laudio_mic_read_level},
    {"snr_thd",           laudio_snr_thd},
    {"play_tone",         laudio_play_tone},
    {"play_wav",          laudio_play_wav},
    /* Streaming API for LLM skill authoring and cap_audio_stream */
    {"start_record",      laudio_start_record},
    {"read_chunk",        laudio_read_chunk},
    {"stop_record",       laudio_stop_record},
    {"start_play",        laudio_start_play},
    {"write_chunk_play",  laudio_write_chunk_play},
    {"stop_play",         laudio_stop_play},
    {NULL, NULL}
};

LUAMOD_API int luaopen_audio(lua_State *L)
{
    /* Input handle metatable */
    luaL_newmetatable(L, AUDIO_META);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, laudio_methods, 0);
    lua_pushcfunction(L, laudio_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    /* Output handle metatable */
    luaL_newmetatable(L, AUDIO_OUT_META);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, laudio_out_methods, 0);
    lua_pushcfunction(L, laudio_out_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    luaL_newlib(L, laudio_funcs);
    return 1;
}
