/*
 * gpsp app support — AV pipeline
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "audio_driver.h"
#include "av_pipeline.h"
#include "sound.h"
#include "video.h"
#include "video_driver.h"

#define GBA_FRAME_RATE               59.7275f
#define AV_PIPELINE_DEPTH            2
#define AUDIO_FRAME_SAMPLES_MAX      ((GBA_SOUND_FREQUENCY / 50) + 1)

static const char *TAG = "gpsp_av";

static GPSP_EXTRAM_BSS u16 gba_framebuffers[AV_PIPELINE_DEPTH][GBA_SCREEN_WIDTH * (GBA_SCREEN_HEIGHT + 1)] __attribute__((aligned(64)));
static int16_t audio_buffers[AV_PIPELINE_DEPTH][AUDIO_FRAME_SAMPLES_MAX * 2];
static uint32_t audio_buffer_frames[AV_PIPELINE_DEPTH];
static bool skip_video_submit[AV_PIPELINE_DEPTH];
static QueueHandle_t av_free_queue;
static QueueHandle_t av_ready_queue;
static bool audio_enabled;
static float audio_frame_samples;
static float audio_frame_fraction;

static uint32_t collect_audio_frame(int16_t *audio_buf, size_t audio_buf_frames)
{
    uint32_t frames_to_read;
    uint32_t frames_produced;

    if (!audio_enabled) {
        return 0;
    }

    frames_to_read = (uint32_t)audio_frame_samples;
    audio_frame_fraction += audio_frame_samples - (float)frames_to_read;
    if (audio_frame_fraction >= 1.0f) {
        frames_to_read++;
        audio_frame_fraction -= 1.0f;
    }

    if (frames_to_read > audio_buf_frames) {
        frames_to_read = (uint32_t)audio_buf_frames;
    }

    frames_produced = sound_read_samples(audio_buf, frames_to_read);
    if (frames_produced < frames_to_read) {
        memset(audio_buf + (frames_produced * 2), 0,
               (frames_to_read - frames_produced) * 2 * sizeof(*audio_buf));
    }

    return frames_to_read;
}

static void av_output_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "AV output task started on core %d", xPortGetCoreID());

    while (1) {
        uint32_t slot_index;

        if (xQueueReceive(av_ready_queue, &slot_index, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!skip_video_submit[slot_index]) {
            esp_err_t err = video_driver_submit_frame(gba_framebuffers[slot_index]);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Video submit failed: %s", esp_err_to_name(err));
            }
        }

        if (audio_enabled && audio_buffer_frames[slot_index] > 0) {
            esp_err_t err = audio_driver_write(audio_buffers[slot_index],
                                               audio_buffer_frames[slot_index]);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Audio write failed: %s", esp_err_to_name(err));
                audio_enabled = false;
            }
        }

        xQueueSend(av_free_queue, &slot_index, portMAX_DELAY);
    }
}

esp_err_t av_pipeline_init(const av_pipeline_config_t *config)
{
    uint32_t slot_index;
    BaseType_t task_ret;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (av_free_queue || av_ready_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    av_free_queue = xQueueCreate(AV_PIPELINE_DEPTH, sizeof(slot_index));
    av_ready_queue = xQueueCreate(AV_PIPELINE_DEPTH, sizeof(slot_index));
    if (!av_free_queue || !av_ready_queue) {
        return ESP_ERR_NO_MEM;
    }

    audio_enabled = config->audio_enabled;
    audio_frame_samples = (float)GBA_SOUND_FREQUENCY / GBA_FRAME_RATE;
    audio_frame_fraction = 0.0f;

    for (slot_index = 0; slot_index < AV_PIPELINE_DEPTH; slot_index++) {
        audio_buffer_frames[slot_index] = 0;
        skip_video_submit[slot_index] = false;
        xQueueSend(av_free_queue, &slot_index, 0);
    }

    task_ret = xTaskCreatePinnedToCore(
        av_output_task,
        "gba_av",
        config->task_stack_size,
        NULL,
        config->task_priority,
        NULL,
        config->output_core
    );
    if (task_ret != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

u16 *av_pipeline_default_video_buffer(void)
{
    return gba_framebuffers[0];
}

esp_err_t av_pipeline_acquire_slot(uint32_t *slot_index, u16 **video_buffer,
                                   TickType_t timeout)
{
    uint32_t local_slot_index;

    if (!slot_index || !video_buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!av_free_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueReceive(av_free_queue, &local_slot_index, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *slot_index = local_slot_index;
    *video_buffer = gba_framebuffers[local_slot_index];
    return ESP_OK;
}

esp_err_t av_pipeline_release_slot(uint32_t slot_index, TickType_t timeout)
{
    if (slot_index >= AV_PIPELINE_DEPTH) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!av_free_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(av_free_queue, &slot_index, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t av_pipeline_submit_slot(uint32_t slot_index, bool skip_video,
                                  TickType_t timeout)
{
    if (slot_index >= AV_PIPELINE_DEPTH) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!av_ready_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    skip_video_submit[slot_index] = skip_video;
    audio_buffer_frames[slot_index] = collect_audio_frame(audio_buffers[slot_index],
                                                          AUDIO_FRAME_SAMPLES_MAX);

    if (xQueueSend(av_ready_queue, &slot_index, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

bool av_pipeline_audio_enabled(void)
{
    return audio_enabled;
}