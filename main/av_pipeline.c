/*
 * gpsp app support — AV pipeline
 *
 * Depth-2 pipeline: the LCD panel owns its own triple-buffered DPI
 * framebuffers, and the emulator keeps two GBA render buffers. PPA is
 * fired asynchronously after each frame and awaited at the start of
 * the next frame, so emulation can render into the alternate GBA buffer
 * while PPA DMA is still consuming the previous one.
 *
 * Audio uses a pipelined approach mirroring the video path: the
 * emulation core collects samples from the ring buffer and hands them
 * to a dedicated audio task on the service core, which performs the
 * (costly) resample + I2S DMA write.  The emulation core waits for
 * the previous audio frame to finish at the start of the NEXT frame,
 * so that emulation and audio processing overlap.
 *
 * Backpressure is inherent: if I2S DMA is full, the audio task blocks
 * in i2s_write, and the emulation core blocks waiting for the audio
 * task — naturally pacing emulation to real-time.  Dynamic rate
 * control adjusts the resample ratio to minimise this blocking.
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
#define AUDIO_BUFFER_COUNT           2
#define AUDIO_TASK_STACK_SIZE        6144
#define AUDIO_TASK_PRIORITY          (configMAX_PRIORITIES - 2)

/* Dynamic rate control.
 *
 * Two-layer approach:
 *   A) Sliding-window average of per-frame production over 64 frames
 *      (~1.07 s) gives a stable estimate of the emulator's true source
 *      rate.  From this we derive the ideal resample step.
 *   B) A small proportional correction based on the water-level error
 *      (current pending − target) nudges the step so the buffer stays
 *      near the target level.  This prevents drift and keeps latency low.
 *
 * The final step is EMA-smoothed (alpha ≈ 1/16) for a silky-smooth
 * transition — no audible pitch steps.
 *
 * Nominal: source 65536 Hz → output 64000 Hz → step = 67109 (Q16).
 * One video-frame of audio ≈ 2196 stereo samples. */
#define DRC_WINDOW_FRAMES    64
#define DRC_WINDOW_SHIFT     6          /* log2(64) */
#define DRC_TARGET_LEVEL     ((int32_t)(2.0f * (float)GBA_SOUND_FREQUENCY / GBA_FRAME_RATE))
#define DRC_LEVEL_GAIN_SHIFT 8          /* P-gain on water-level error → step nudge */
#define DRC_STEP_SMOOTH_SHIFT 4         /* EMA alpha ≈ 1/16 for final step */

static const char *TAG = "gpsp_av";

static GPSP_EXTRAM_BSS u16 gba_framebuffers[GBA_RENDER_FB_COUNT][GBA_SCREEN_WIDTH * (GBA_SCREEN_HEIGHT + 1)] __attribute__((aligned(64)));
static GPSP_EXTRAM_BSS int16_t audio_buffers[AUDIO_BUFFER_COUNT][AUDIO_FRAME_SAMPLES_MAX * 2];
static uint32_t audio_write_idx;        /* which buffer the emu core fills */
static float audio_frame_samples;
static float audio_frame_fraction;
static uint32_t current_render_fb;
static bool video_frame_pending;
static bool audio_frame_pending;

/* Audio task. */
static TaskHandle_t audio_task_handle;
static volatile uint32_t audio_pending_frames; /* frames to write (set by emu core) */
static volatile int16_t *audio_pending_buf;    /* buffer pointer for audio task */
static TaskHandle_t audio_caller_task;         /* emu core task, for completion signal */

/* DRC state — owned by emulation core. */
static uint32_t drc_nominal_step_q16;
static int32_t  drc_nominal_prod_q8;    /* nominal per-frame production (Q8) */
static int32_t  drc_step_q24;           /* EMA-smoothed final step (Q24) */

/* Sliding window for production-rate estimation. */
static int32_t  drc_window[DRC_WINDOW_FRAMES];
static uint32_t drc_window_idx;
static int32_t  drc_window_sum;
static uint32_t drc_window_count;
static uint32_t drc_last_pending;

static uint32_t collect_audio_frame(int16_t *audio_buf, size_t audio_buf_frames)
{
    uint32_t frames_to_read;
    uint32_t frames_produced;

    if (!audio_enabled) {
        return 0;
    }

    /* Fixed read size: exactly one video-frame's worth of audio. */
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

    return frames_produced;
}

/* Audio task: resample + i2s_write on the service core. */
static void audio_output_task(void *arg)
{
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint32_t frames = audio_pending_frames;
        int16_t *buf = (int16_t *)audio_pending_buf;

        if (frames > 0 && buf != NULL) {
            esp_err_t err = audio_driver_write(buf, frames);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Audio write failed: %s", esp_err_to_name(err));
            }
        }

        /* Signal completion to the emulation core. */
        xTaskNotifyGive(audio_caller_task);
    }
}

/* Estimates the emulator's true source sample rate from a sliding
 * window of per-frame production and applies a water-level correction.
 * Called synchronously from the emulation core each frame. */
static void audio_dynamic_rate_control(uint32_t frames_consumed)
{
    uint32_t pending = sound_samples_pending();

    /* Net samples produced since last call:
     *   produced = consumed_this_frame + delta_pending */
    int32_t pending_delta = (int32_t)(pending - drc_last_pending);
    int32_t produced = (int32_t)(frames_consumed * 2) + pending_delta;
    drc_last_pending = pending;

    if (produced <= 0) {
        return;
    }

    /* ── Layer A: sliding-window average production rate ── */
    drc_window_sum -= drc_window[drc_window_idx];
    drc_window[drc_window_idx] = produced;
    drc_window_sum += produced;
    drc_window_idx = (drc_window_idx + 1) & (DRC_WINDOW_FRAMES - 1);
    if (drc_window_count < DRC_WINDOW_FRAMES) {
        drc_window_count++;
    }

    /* Average production per frame (Q8 for sub-sample precision). */
    int32_t avg_prod_q8 = (drc_window_sum << 8) / (int32_t)drc_window_count;

    /* Ideal step to match the measured source rate:
     *   step = nominal_step × (avg_production / nominal_production)
     * Computed in Q24 for smooth sub-LSB resolution. */
    int64_t num = (int64_t)drc_nominal_step_q16 * avg_prod_q8;
    int32_t rate_step_q24 = (int32_t)((num << 8) / drc_nominal_prod_q8);

    /* ── Layer B: water-level P-correction ──
     * Nudge the step so that the buffer converges to the target level.
     *   level too high → increase step → fewer output samples → drain
     *   level too low  → decrease step → more output samples → fill
     * Correction is small (gain ≈ 1/256 of error in Q24 step units)
     * so it never overrides the rate estimate, just trims drift. */
    int32_t level_error = (int32_t)pending - DRC_TARGET_LEVEL;
    int32_t level_correction_q24 = (level_error << 8) >> DRC_LEVEL_GAIN_SHIFT;

    int32_t target_step_q24 = rate_step_q24 + level_correction_q24;

    /* ── Smooth the final step (EMA, alpha ≈ 1/16) ── */
    drc_step_q24 += (target_step_q24 - drc_step_q24) >> DRC_STEP_SMOOTH_SHIFT;

    /* Apply Q24 → delta from nominal Q16. */
    int32_t delta = (drc_step_q24 >> 8) - (int32_t)drc_nominal_step_q16;
    audio_driver_adjust_rate(delta);
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
    audio_frame_pending = false;
    audio_write_idx = 0;
    audio_caller_task = NULL;
    drc_nominal_prod_q8 = (int32_t)((2.0f * (float)GBA_SOUND_FREQUENCY
                                     / GBA_FRAME_RATE) * 256.0f);
    drc_nominal_step_q16 = (uint32_t)(((uint64_t)GBA_SOUND_FREQUENCY << 16)
                                      / CONFIG_GPSP_AUDIO_SAMPLE_RATE);
    drc_step_q24 = (int32_t)drc_nominal_step_q16 << 8;
    drc_last_pending = 0;
    memset(drc_window, 0, sizeof(drc_window));
    drc_window_idx = 0;
    drc_window_sum = 0;
    drc_window_count = 0;

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
        } else {
            ESP_LOGI(TAG, "Audio: pipelined DRC on core %d (source %lu -> output %lu Hz)",
                     (int)core,
                     (unsigned long)GBA_SOUND_FREQUENCY,
                     (unsigned long)CONFIG_GPSP_AUDIO_SAMPLE_RATE);
        }
    }

    ESP_LOGI(TAG, "AV pipeline initialised");
    return ESP_OK;
}

u16 *av_pipeline_video_buffer(void)
{
    return gba_framebuffers[current_render_fb];
}

void av_pipeline_wait_for_previous_frame(void)
{
    if (video_frame_pending) {
        video_driver_await_frame();
        video_frame_pending = false;
    }

    /* Wait for the previous audio frame's resample + i2s_write to
     * finish on the service core.  If I2S DMA was full, this is
     * where we block — propagating backpressure to the emu core. */
    if (audio_frame_pending) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        audio_frame_pending = false;
    }
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
        /* Capture the calling task handle on the first call so the
         * audio task notifies the correct (emulation) task. */
        if (audio_caller_task == NULL) {
            audio_caller_task = xTaskGetCurrentTaskHandle();
        }

        /* Collect samples on the emu core (cheap: just memcpy from ring
         * buffer).  DRC also runs here since it's just arithmetic. */
        int16_t *buf = audio_buffers[audio_write_idx];
        uint32_t frames = collect_audio_frame(buf, AUDIO_FRAME_SAMPLES_MAX);
        if (frames > 0) {
            audio_dynamic_rate_control(frames);

            /* Hand off to the audio task for resample + i2s_write. */
            audio_pending_buf = buf;
            audio_pending_frames = frames;
            audio_write_idx = (audio_write_idx + 1) % AUDIO_BUFFER_COUNT;
            xTaskNotifyGive(audio_task_handle);
            audio_frame_pending = true;
        }
    }

    return ESP_OK;
}

bool av_pipeline_audio_enabled(void)
{
    return audio_enabled;
}

int32_t av_pipeline_audio_speed_pcnt_x100(void)
{
    /* Return speed relative to nominal as percent×100 (10000 = 100.00%).
     * drc_step_q24 / (nominal_step_q16 << 8) is the resample ratio;
     * the inverse is the playback speed ratio. */
    if (!audio_enabled || drc_nominal_step_q16 == 0)
        return 10000;
    int64_t nominal_q24 = (int64_t)drc_nominal_step_q16 << 8;
    /* speed = nominal / actual  →  percent×100 = nominal * 1000000 / actual */
    return (int32_t)(nominal_q24 * 10000 / drc_step_q24);
}