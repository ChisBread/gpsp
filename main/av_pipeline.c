/*
 * gpsp app support — AV pipeline
 *
 * Depth-2 pipeline: the LCD panel owns its own triple-buffered DPI
 * framebuffers, and the emulator keeps two GBA render buffers.
 *
 * All heavyweight per-frame work — PPA submit, audio collection,
 * dynamic rate control, resampling, I2S DMA write, PPA await, and
 * VSYNC pacing — runs on a single "AV output task" pinned to the
 * service core.  The emulation core only flips a buffer pointer and
 * kicks the task via xTaskNotifyGive, then immediately starts
 * rendering the next frame.  At the beginning of the NEXT frame,
 * av_pipeline_wait_for_previous_frame() blocks until the AV task
 * signals completion, providing natural backpressure.
 *
 * On the service core the order is:
 *   1. video_driver_submit_frame  — kick PPA DMA (async, ~300 µs setup)
 *   2. collect_audio + DRC        — while PPA hardware runs in parallel
 *   3. audio_driver_write         — resample + I2S (PPA still parallel)
 *   4. video_driver_await_frame   — PPA likely done; draw_bitmap + VSYNC
 *
 * This keeps PPA DMA, audio processing, and I2S DMA overlapped,
 * and the emulation core is 100 % free for GBA computation.
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
#define AV_TASK_STACK_SIZE           8192
#define AV_TASK_PRIORITY             (configMAX_PRIORITIES - 2)

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
static GPSP_EXTRAM_BSS int16_t audio_buffer[AUDIO_FRAME_SAMPLES_MAX * 2];
static float audio_frame_samples;
static float audio_frame_fraction;
static uint32_t current_render_fb;

/* Unified AV output task. */
static TaskHandle_t av_task_handle;
static TaskHandle_t av_caller_task;            /* emu core task handle */
static volatile const u16 *av_pending_fb;      /* framebuffer for PPA */
static volatile bool av_pending_skip_video;
static bool av_frame_pending;

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

/* Forward declaration — defined below av_output_task. */
static void audio_dynamic_rate_control(uint32_t frames_consumed);

/* Unified AV output task: PPA + audio + VSYNC on the service core.
 *
 * Runs one iteration per emulated frame.  The order is chosen so that
 * PPA DMA runs in the hardware background while audio is processed:
 *   1. Kick PPA async  (hardware busy ─────────────────────────┐)
 *   2. Collect audio from ring buffer                          │
 *   3. DRC arithmetic                                          │
 *   4. Resample + I2S DMA write (blocks if DMA full)           │
 *   5. Await PPA completion  (usually instant by now) ─────────┘
 *   6. draw_bitmap + VSYNC pacing
 *   7. Signal emulation core
 */
static void av_output_task(void *arg)
{
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        bool skip_video = av_pending_skip_video;

        /* 1. Kick PPA hardware (async DMA). */
        if (!skip_video) {
            esp_err_t err = video_driver_submit_frame((const uint16_t *)av_pending_fb);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Video submit failed: %s", esp_err_to_name(err));
            }
        }

        /* 2-4. Audio: collect → DRC → resample + I2S.
         * PPA DMA runs in parallel during this entire block. */
        if (audio_enabled) {
            uint32_t frames = collect_audio_frame(audio_buffer,
                                                  AUDIO_FRAME_SAMPLES_MAX);
            if (frames > 0) {
                audio_dynamic_rate_control(frames);

                esp_err_t err = audio_driver_write(audio_buffer, frames);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Audio write failed: %s",
                             esp_err_to_name(err));
                }
            }
        }

        /* 5-6. Await PPA + VSYNC.  PPA is almost certainly finished
         * by now, so this is mainly the VSYNC wait. */
        if (!skip_video) {
            video_driver_await_frame();
        }

        /* 7. Signal the emulation core that this frame is done. */
        xTaskNotifyGive(av_caller_task);
    }
}

/* Estimates the emulator's true source sample rate from a sliding
 * window of per-frame production and applies a water-level correction.
 * Called from the AV output task on the service core each frame. */
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
    av_frame_pending = false;
    av_caller_task = NULL;
    av_task_handle = NULL;
    drc_nominal_prod_q8 = (int32_t)((2.0f * (float)GBA_SOUND_FREQUENCY
                                     / GBA_FRAME_RATE) * 256.0f);
    drc_nominal_step_q16 = (uint32_t)(((uint64_t)GBA_SOUND_FREQUENCY << 16)
                                      / CONFIG_GPSP_AUDIO_SAMPLE_RATE);
    drc_step_q24 = (int32_t)drc_nominal_step_q16 << 8;
    drc_last_pending = 0;
    /* Pre-seed the DRC window with production matching the actual
     * emulator speed (~60.5 Hz) rather than the nominal GBA rate
     * (59.7275 Hz).  This gives the DRC an initial "boost" so the
     * resample step is already close to the real operating point,
     * avoiding a ~1-second convergence period that causes audible
     * buffer oscillation and FPS jitter.
     *
     * Factor: 60.5 / 59.7275 ≈ 1.0129 → per-frame production is
     * inflated by ~1.3% which translates to a ~1.3% higher initial
     * resample step.  The DRC will refine from here. */
    {
        int32_t nom = (int32_t)(2.0f * (float)GBA_SOUND_FREQUENCY / GBA_FRAME_RATE);
        int32_t seed = (int32_t)((float)nom * (60.5f / GBA_FRAME_RATE));
        for (uint32_t i = 0; i < DRC_WINDOW_FRAMES; i++)
            drc_window[i] = seed;
        drc_window_idx = 0;
        drc_window_sum = seed * DRC_WINDOW_FRAMES;
        drc_window_count = DRC_WINDOW_FRAMES;
        /* Also bias the EMA step so it's consistent with the window. */
        int64_t num = (int64_t)drc_nominal_step_q16 * (seed << 8);
        drc_step_q24 = (int32_t)((num << 8) / drc_nominal_prod_q8);
    }

    /* Create the unified AV output task on the service core. */
    {
        BaseType_t core = (CONFIG_GPSP_EMULATION_CORE == 0) ? 1 : 0;
        BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
            av_output_task, "av_out",
            AV_TASK_STACK_SIZE, NULL,
            AV_TASK_PRIORITY,
            &av_task_handle,
            core,
            MALLOC_CAP_SPIRAM);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "AV task creation failed");
            av_task_handle = NULL;
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "AV output task on core %d (audio %s, source %lu -> output %lu Hz)",
                 (int)core,
                 audio_enabled ? "on" : "off",
                 (unsigned long)GBA_SOUND_FREQUENCY,
                 (unsigned long)CONFIG_GPSP_AUDIO_SAMPLE_RATE);
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
    /* Block until the AV output task finishes the previous frame's
     * PPA + audio + VSYNC.  If the emulator is faster than real-time,
     * this is where it stalls — backpressure from I2S/VSYNC. */
    if (av_frame_pending) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        av_frame_pending = false;
    }
}

esp_err_t av_pipeline_submit_frame(bool skip_video)
{
    if (!av_task_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Capture the calling task handle on the first call so the
     * AV task notifies the correct (emulation) task. */
    if (av_caller_task == NULL) {
        av_caller_task = xTaskGetCurrentTaskHandle();
    }

    /* Set up the work descriptor for the AV task. */
    if (!skip_video) {
        av_pending_fb = gba_framebuffers[current_render_fb];
        current_render_fb = (current_render_fb + 1) % GBA_RENDER_FB_COUNT;
    }
    av_pending_skip_video = skip_video;

    /* Kick the AV output task and return immediately — the emulation
     * core is now free to render the next frame into the alternate
     * GBA buffer while PPA + audio run on the service core. */
    xTaskNotifyGive(av_task_handle);
    av_frame_pending = true;

    return ESP_OK;
}

bool av_pipeline_audio_enabled(void)
{
    return audio_enabled;
}

int32_t av_pipeline_audio_speed_pcnt_x100(void)
{
    /* Return speed relative to nominal as percent×100 (10000 = 100.00%).
     * A higher resample step consumes more source per output sample,
     * i.e. plays back faster.  speed = actual_step / nominal_step. */
    if (!audio_enabled || drc_nominal_step_q16 == 0)
        return 10000;
    int64_t nominal_q24 = (int64_t)drc_nominal_step_q16 << 8;
    return (int32_t)((int64_t)drc_step_q24 * 10000 / nominal_q24);
}