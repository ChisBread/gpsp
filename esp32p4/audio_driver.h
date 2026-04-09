/*
 * gpsp ESP32-P4 Platform — Audio driver header
 * ES8311 Codec (I2C control + I2S data) with speaker PA
 * Target board: JC4880P443C_I_W
 */

#ifndef ESP32P4_AUDIO_DRIVER_H
#define ESP32P4_AUDIO_DRIVER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* JC4880 audio hardware pins */
#define AUDIO_I2S_MCLK_GPIO   13
#define AUDIO_I2S_BCLK_GPIO   12
#define AUDIO_I2S_WS_GPIO     10
#define AUDIO_I2S_DOUT_GPIO    9   /* Speaker data */
#define AUDIO_I2S_DIN_GPIO    48   /* Mic data (ES7210), unused for playback */
#define AUDIO_PA_GPIO         11   /* Speaker power amp enable */
#define AUDIO_I2C_SDA_GPIO     7
#define AUDIO_I2C_SCL_GPIO     8

/* ES8311 I2C address */
#define ES8311_I2C_ADDR       0x18
#define AUDIO_PCM_SOURCE_RATE 65536u

/* Audio configuration */
typedef struct {
    uint32_t sample_rate;      /* Hardware output sample rate (e.g., 32000, 44100, 64000) */
    uint32_t source_sample_rate; /* Incoming PCM sample rate before resampling */
} audio_driver_config_t;

/**
 * Initialize I2C + I2S + ES8311 codec for audio playback.
 */
esp_err_t audio_driver_init(const audio_driver_config_t *config);

/**
 * Write audio samples to the I2S DMA buffer.
 * Samples are interleaved stereo signed 16-bit PCM (L, R, L, R, ...).
 *
 * @param samples  Pointer to sample data
 * @param count    Number of stereo sample pairs
 */
esp_err_t audio_driver_write(const int16_t *samples, size_t count);

/**
 * Set speaker volume (0-100).
 */
esp_err_t audio_driver_set_volume(int volume_percent);

/**
 * Deinitialize audio driver.
 */
void audio_driver_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP32P4_AUDIO_DRIVER_H */
