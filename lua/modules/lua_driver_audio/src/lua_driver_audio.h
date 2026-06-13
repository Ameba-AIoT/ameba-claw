/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_audio(lua_State *L);

/**
 * Record raw PCM from DMIC to a file (no WAV header).
 * @param path        VFS path for output file (e.g. "vfs:/tmp/rec.pcm")
 * @param sample_rate Hz  (8000 / 16000 / 44100 / 48000)
 * @param channels    1 or 2
 * @param duration_ms recording duration in milliseconds
 * @return 0 on success, -1 on error
 */
int audio_record_pcm_c(const char *path, uint32_t sample_rate,
                       uint8_t channels, uint32_t duration_ms);

/* C-level DMIC streaming API — used by cap_audio_stream (no Lua state needed) */
int  audio_dmic_open(uint32_t sample_rate, uint8_t channels,
                     uint32_t dmic_clk_pin, uint32_t dmic_data_pin);
/* Returns AUDIO_CHUNK_SIZE on success, 0 on timeout. chunk_out points into
 * internal DMA ping-pong buffer — valid until the next call to this function. */
int  audio_dmic_read_chunk(const uint8_t **chunk_out, uint32_t timeout_ms);
void audio_dmic_close(void);

/* C-level speaker (I2S TX) streaming API — used by cap_audio_stream */
/* Default I2S output pins: PA_9 MCLK, PA_10 BCLK, PA_14 LRCLK, PA_16 DATA0
 * (RTL8721F eval board with external I2S DAC, e.g. MAX98357A) */
int  audio_sp_open(uint32_t sample_rate, uint8_t channels);
/* Copy data into DMA buffer and play; blocks until previous chunk finishes.
 * len is clamped to AUDIO_CHUNK_SIZE (4096 bytes). Returns 0 on success. */
int  audio_sp_write_chunk(const uint8_t *data, uint32_t len);
void audio_sp_close(void);

#ifdef __cplusplus
}
#endif
