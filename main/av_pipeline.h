/*
 * gpsp app support — AV pipeline
 */

#ifndef GPSP_MAIN_AV_PIPELINE_H
#define GPSP_MAIN_AV_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool audio_enabled;
} av_pipeline_config_t;

esp_err_t av_pipeline_init(const av_pipeline_config_t *config);
u16 *av_pipeline_video_buffer(void);
void av_pipeline_wait_for_previous_frame(void);
esp_err_t av_pipeline_submit_frame(bool skip_video);
bool av_pipeline_audio_enabled(void);
void av_pipeline_audio_buffered(uint32_t *out_queued, uint32_t *out_total);

/**
 * Snapshot the display framebuffer into a pre-allocated destination.
 * Arms a request; the actual memcpy runs in the AV task on the service
 * core after VSYNC — same core as httpd, avoiding L1 DCache coherency
 * issues.  Blocks until the next VSYNC.
 * @param dst       Pre-allocated buffer (>= 240*160*2 bytes, PSRAM OK)
 * @param dst_size  Size of dst in bytes
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if no VSYNC within 100 ms
 */
esp_err_t av_pipeline_snapshot_frame(uint8_t *dst, size_t dst_size);

/**
 * Start tee-ing collected audio into a StreamBuffer for the streaming task.
 * Call once when streaming starts. Does nothing if already enabled.
 */
esp_err_t av_pipeline_stream_audio_start(void);

/**
 * Stop tee-ing audio to the stream buffer and free resources.
 */
void av_pipeline_stream_audio_stop(void);

/**
 * Read accumulated audio from the stream tee (non-blocking).
 * @param out       Output buffer for s16 interleaved stereo samples
 * @param max_bytes Maximum bytes to read
 * @return          Actual bytes read (0 if nothing available)
 */
size_t av_pipeline_stream_audio_read(int16_t *out, size_t max_bytes);

#ifdef __cplusplus
}
#endif

#endif /* GPSP_MAIN_AV_PIPELINE_H */