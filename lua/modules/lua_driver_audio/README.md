# lua_driver_audio — DMIC & Speaker Driver

Lua audio driver for **RTL8721F**.  
Provides `require("audio")` (DMIC input) and integrates the MAX98357A I²S speaker via `aplay`.

---

## Hardware Wiring

### DMIC (input)
| Signal    | Pin  | Pad  |
|-----------|------|------|
| DMIC_CLK  | PB3  | 0x23 |
| DMIC_DATA | PB4  | 0x24 |

### Speaker — MAX98357A (output, I²S)
| Signal       | Pin  | Pad  |
|--------------|------|------|
| I²S BCLK     | PA25 | 0x19 |
| I²S WS/LRCK  | PA26 | 0x1A |
| I²S DIO3/DOUT| PA29 | 0x1D |

Pin mux is configured in `ameba_audio_hw_usrcfg.h`, not in driver code.

---

## AT Commands

| Command | Description |
|---------|-------------|
| `AT+CLAW=speaker` | Full speaker test: help → 1 kHz sine (20 s) → nokia.wav |
| `AT+CLAW=speaker,sine` | 1 kHz sine wave only (20 s) |
| `AT+CLAW=speaker,nokia` | Play nokia.wav from VFS only |
| `AT+CLAW=dmic` | DMIC SNR/THD test (speaker vol=0.4, 5 iterations) |
| `AT+CLAW=dmic,<vol>` | DMIC test with custom speaker volume (0.0–1.0) |
| `AT+CLAW=dmic,ext` | DMIC test using external 1 kHz source (no speaker) |

---

## Lua API (`require("audio")`)

### `audio.new_input(sr, ch, bps [, adc_vol [, clk_pin, data_pin]])`
Open DMIC for recording.

| Param | Type | Description |
|-------|------|-------------|
| `sr` | integer | Sample rate (Hz): 8000 / 16000 / 44100 / 48000 |
| `ch` | integer | Channels: 1 (mono) or 2 (stereo) |
| `bps` | integer | Bits per sample: 16 (only supported value) |
| `adc_vol` | integer | ADC digital gain 0x00–0x7F (default 0x2F) |
| `clk_pin` | integer | DMIC_CLK pad index (0 = skip pinmux) |
| `data_pin` | integer | DMIC_DATA pad index (0 = skip pinmux) |

Returns `handle` or `nil, errmsg`.

### `audio.close(h)`
Stop DMA and release hardware. Returns `true`.

### `audio.record_wav(h, path, duration_ms)`
Record PCM audio to a WAV file.  
Returns `true` or `nil, errmsg`.

### `audio.set_gain(h, vol)`
Set ADC digital volume (0x00–0x7F). Returns `true`.

### `audio.set_mute(h, bool)`
Mute or unmute ADC. Returns `true`.

### `audio.mic_read_level(h)`
Read one DMA chunk and return RMS level (0–32767).

### `audio.snr_thd(h)`
Run a 512-point FFT on one DMA chunk.  
Returns `snr1_dB, thd1_dB, snr2_dB, thd2_dB, pass`.

- **SNR**: signal-to-noise ratio (harmonics excluded from noise floor)
- **THD**: total harmonic distortion (SINAD — harmonics included)
- **pass**: `true` when both channels have SNR ≥ 20 dB and THD ≥ 20 dB

---

## Nokia WAV — Automatic Provisioning

`nokia_via.wav` (Nokia ringtone, 8 kHz / mono / 16-bit PCM, ~47 KB) is embedded as a C byte array in `test/nokia_wav.h`. On every boot, `lua_task()` calls `lua_driver_audio_speaker_provision()` which writes the data to `vfs:nokia.wav`. No separate `vfs_nokia.bin` image needs to be flashed manually.

---

## DMIC SNR/THD Test Details

- Sample rate: **16 kHz** in external-source mode (1 kHz = bin 32, no leakage)  
- Sample rate: **48 kHz** in speaker mode (SPORT0 shared with speaker at 48 kHz; Hann window suppresses leakage)
- ADC gain: 0x7F (ext) / 0x3F (speaker mode)
- Passes if ≥ 3 of 5 iterations report SNR ≥ 20 dB and THD ≥ 20 dB on both channels
- **DMIC must be opened before `aplay`** starts, so `AUDIO_SP_Reset` happens before the speaker registers SPORT0 TX

---

## File Layout

```
lua_driver_audio/
├── src/
│   ├── lua_driver_audio.h      # luaopen_audio declaration
│   └── lua_driver_audio.c      # DMIC driver + SNR/THD + Lua bindings
└── test/
    ├── nokia_wav.h             # Embedded WAV binary (auto-generated, do not edit)
    ├── lua_speaker_test_provision.c  # Speaker test script + AT+CLAW=speaker handler
    └── lua_dmic_test_provision.c     # DMIC test script + AT+CLAW=dmic handler
```
