/*
 * gpsp app support — AV pipeline
 *
 * Depth-2 pipeline: the LCD panel owns its own triple-buffered DPI
 * framebuffers, and the emulator keeps two GBA render buffers. PPA is
 * fired asynchronously after each frame and awaited at the start of
 * the next frame, so emulation can render into the alternate GBA buffer
 * while PPA DMA is still consuming the previous one.
 *
 * Audio is offloaded to a dedicated FreeRTOS task on the service core
 * so that sound_read_samples + resample + i2s_write (~1 ms) do not
 * block the emulation core.
 */

#include <string.h>

#include "av_pipeline.h"
#include "common.h"

static bool audio_enabled;

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_driver.h"
#include "sound.h"
#include "video.h"
#include "video_driver.h"

#define GBA_FRAME_RATE               59.7275f
#define GBA_RENDER_FB_COUNT          2
#define AUDIO_FRAME_SAMPLES_MAX      ((GBA_SOUND_FREQUENCY / 50) + 1)
#define AUDIO_TASK_STACK_SIZE        6144
#define AUDIO_TASK_PRIORITY          (configMAX_PRIORITIES - 2)

static const char *TAG = "gpsp_av";

static GPSP_EXTRAM_BSS u16 gba_framebuffers[GBA_RENDER_FB_COUNT][GBA_SCREEN_WIDTH * (GBA_SCREEN_HEIGHT + 1)] __attribute__((aligned(64)));
static GPSP_EXTRAM_BSS int16_t audio_buffer[AUDIO_FRAME_SAMPLES_MAX * 2];
static float audio_frame_samples;
static float audio_frame_fraction;
static uint32_t current_render_fb;
static bool video_frame_pending;

static TaskHandle_t audio_task_handle;

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

static void audio_output_task(void *arg)
{
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!audio_enabled) {
            continue;
        }

        uint32_t frames = collect_audio_frame(audio_buffer,
                                              AUDIO_FRAME_SAMPLES_MAX);
        if (frames > 0) {
            esp_err_t err = audio_driver_write(audio_buffer, frames);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Audio write failed: %s", esp_err_to_name(err));
                audio_enabled = false;
            }
        }
    }
}

esp_err_t av_pipeline_init(const av_pipeline_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_enabled = config->audio_enabled;
    audio_frame_samples = (float)GBA_SOUND_FREQUENCY / GBA_FRAME_RATE;
    audio_frame_fraction = 0.0f;
    current_render_fb = 0;
    video_frame_pending = false;

    if (audio_enabled) {
        BaseType_t core = (CONFIG_GPSP_EMULATION_CORE == 0) ? 1 : 0;
        BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
            audio_output_task, "av_audio",
            AUDIO_TASK_STACK_SIZE, NULL,
            AUDIO_TASK_PRIORITY,
            &audio_task_handle,
            core,
            MALLOC_CAP_SPIRAM);
        if (ret != pdPASS) {
            ESP_LOGW(TAG, "Audio task creation failed; audio disabled");
            audio_enabled = false;
            audio_task_handle = NULL;
        }
    }

    ESP_LOGI(TAG, "AV pipeline initialised (audio task on core %d)",
             (CONFIG_GPSP_EMULATION_CORE == 0) ? 1 : 0);
    return ESP_OK;
}

u16 *av_pipeline_video_buffer(void)
{
    return gba_framebuffers[current_render_fb];
}

void av_pipeline_wait_for_previous_frame(void)
{
    if (!video_frame_pending) {
        return;
    }

    /* Complete the previous frame's PPA + VSYNC pacing before the next
     * submit reuses the video driver's async path. */
    video_driver_await_frame();
    video_frame_pending = false;
}

esp_err_t av_pipeline_submit_frame(bool skip_video)
{
    if (!skip_video) {
        esp_err_t err = video_driver_submit_frame(gba_framebuffers[current_render_fb]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Video submit failed: %s", esp_err_to_name(err));
            return err;
        }
        current_render_fb = (current_render_fb + 1) % GBA_RENDER_FB_COUNT;

        /* The next av_pipeline_wait_for_previous_frame() waits for
         * completion while the emulator is already rendering into the
         * alternate GBA buffer. */
        video_frame_pending = true;
    }

    if (audio_enabled && audio_task_handle) {
        xTaskNotifyGive(audio_task_handle);
    }

    return ESP_OK;
}

bool av_pipeline_audio_enabled(void)
{
    return audio_enabled;
}