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
    size_t task_stack_size;
    BaseType_t output_core;
    UBaseType_t task_priority;
    bool audio_enabled;
} av_pipeline_config_t;

esp_err_t av_pipeline_init(const av_pipeline_config_t *config);
u16 *av_pipeline_default_video_buffer(void);
esp_err_t av_pipeline_acquire_slot(uint32_t *slot_index, u16 **video_buffer,
                                   TickType_t timeout);
esp_err_t av_pipeline_release_slot(uint32_t slot_index, TickType_t timeout);
esp_err_t av_pipeline_submit_slot(uint32_t slot_index, bool skip_video,
                                  TickType_t timeout);
bool av_pipeline_audio_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* GPSP_MAIN_AV_PIPELINE_H */