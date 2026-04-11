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

/* Returns audio playback speed relative to nominal as percent×100.
 * 10000 = 100.00%, 10050 = 100.50%, etc. */
int32_t av_pipeline_audio_speed_pcnt_x100(void);

#ifdef __cplusplus
}
#endif

#endif /* GPSP_MAIN_AV_PIPELINE_H */