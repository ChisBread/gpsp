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
 *   2. collect_audio              — while PPA hardware runs in parallel
 *   3. audio_driver_write         — I2S DMA write (PPA still parallel)
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
#include "freertos/stream_buffer.h"

#include "audio_driver.h"
#include "sound.h"
#include "video.h"
#include "video_driver.h"

#define GBA_RENDER_FB_COUNT          2
#define AUDIO_FRAME_SAMPLES_MAX      ((GBA_SOUND_FREQUENCY / 50) + 1)
#define AV_TASK_STACK_SIZE           8192
#define AV_TASK_PRIORITY             (configMAX_PRIORITIES - 2)

/* Native frame rate derived from the emulated clock.  With OVERCLOCK_60FPS
 * the base rate is fixed so native FPS is exactly 60.000.  Without it,
 * the clock falls back to the original ~16.78 MHz rate and native FPS is
 * the stock GBA cadence.  This is the rate
 * at which the emulator *produces* audio — one GBA frame always covers
 * 228×1232 = 280896 CPU cycles, so per-frame production is
 *   GBA_SOUND_FREQUENCY / GBA_NATIVE_FPS
 * which is ≈ 1092 with OVERCLOCK or ≈ 1097 without.
 *
 * GBA_FRAME_RATE (= display VSYNC rate) is only used for display
 * pacing; audio calculations must use GBA_NATIVE_FPS or the ring
 * buffer slowly drains/fills due to the rate mismatch. */
#define GBA_NATIVE_FPS               (GBC_BASE_RATE / (228.0f * (272.0f + 960.0f)))

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

/* Frame snapshot: web server arms a request (sets s_snap_dst) and
 * blocks on s_snap_sem.  The copy executes in the AV task after VSYNC,
 * on the SERVICE core — same core that later sends via HTTP.
 * This avoids cross-core L1 DCache coherency issues.
 * Cost: ~300 µs memcpy per request, only when web is watching. */
static volatile uint8_t *s_snap_dst;
static volatile size_t   s_snap_size;
static SemaphoreHandle_t s_snap_sem;

/* Stream audio tee: when active, the AV task writes collected audio
 * into this FreeRTOS StreamBuffer.  The streaming web task reads from it.
 * Lock-free for single-producer/single-consumer. */
static StreamBufferHandle_t s_stream_audio_sb;

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

    if (frames_produced < frames_to_read) {
        static uint32_t underrun_count = 0;
        underrun_count++;
        if ((underrun_count & 63) == 1) {  /* log every 64th underrun */
            ESP_LOGW(TAG, "Audio underrun #%lu: got %lu/%lu frames (pending %lu)",
                     (unsigned long)underrun_count,
                     (unsigned long)frames_produced,
                     (unsigned long)frames_to_read,
                     (unsigned long)sound_samples_pending());
        }
    }

    return frames_produced;
}

/* Unified AV output task: PPA + audio + VSYNC on the service core.
 *
 * Runs one iteration per emulated frame.  The order is chosen so that
 * PPA DMA runs in the hardware background while audio is processed:
 *   1. Kick PPA async  (hardware busy ─────────────────────────┐)
 *   2. Collect audio from ring buffer                          │
 *   3. I2S DMA write (blocks if DMA full)                      │
 *   4. Await PPA completion  (usually instant by now) ─────────┘
 *   5. draw_bitmap + VSYNC pacing
 *   6. Signal emulation core
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

        /* 2-4. Audio: collect → DRC → I2S.
         * PPA DMA runs in parallel during this entire block. */
        if (audio_enabled) {
            uint32_t frames = collect_audio_frame(audio_buffer,
                                                  AUDIO_FRAME_SAMPLES_MAX);
            if (frames > 0) {
                esp_err_t err = audio_driver_write(audio_buffer, frames);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Audio write failed: %s",
                             esp_err_to_name(err));
                }
                /* Tee audio to stream buffer (all-or-nothing to avoid
                 * partial writes that corrupt sample boundaries) */
                if (s_stream_audio_sb) {
                    size_t nbytes = frames * 2 * sizeof(int16_t);
                    if (xStreamBufferSpacesAvailable(s_stream_audio_sb) >= nbytes) {
                        xStreamBufferSend(s_stream_audio_sb, audio_buffer,
                                          nbytes, 0);
                    }
                }
            }
        }


        /* Snapshot: copy the display buffer for the web server.
         * This avoids cross-core L1 DCache coherency issues.
         * Cost: ~300 µs memcpy per request, only when web is watching. */
        if (s_snap_dst) {
            memcpy((void *)s_snap_dst, (const void *)av_pending_fb, s_snap_size);
            s_snap_dst = NULL;
            xSemaphoreGive(s_snap_sem);
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

esp_err_t av_pipeline_init(const av_pipeline_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_enabled = config->audio_enabled;
    audio_frame_samples = (float)GBA_SOUND_FREQUENCY / GBA_NATIVE_FPS;
    audio_frame_fraction = 0.0f;
    current_render_fb = 0;
    av_frame_pending = false;
    av_caller_task = NULL;
    av_task_handle = NULL;

    s_snap_dst = NULL;
    if (!s_snap_sem) {
        s_snap_sem = xSemaphoreCreateBinary();
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
        ESP_LOGI(TAG, "AV output task on core %d (audio %s)",
                 (int)core,
                 audio_enabled ? "on" : "off");
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

esp_err_t av_pipeline_snapshot_frame(uint8_t *dst, size_t dst_size)
{
    if (!dst || dst_size == 0 || !s_snap_sem) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t frame_bytes = GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * 2;
    s_snap_size = (dst_size < frame_bytes) ? dst_size : frame_bytes;
    __sync_synchronize();          /* size visible before arming */
    s_snap_dst = dst;              /* arm — submit_frame checks this */

    /* Block until submit_frame() copies the next completed frame. */
    if (xSemaphoreTake(s_snap_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        s_snap_dst = NULL;         /* cancel stale request */
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void av_pipeline_audio_buffered(uint32_t *out_queued, uint32_t *out_total)
{
    if (!audio_enabled) {
        if (out_queued) *out_queued = 0;
        if (out_total)  *out_total  = 0;
        return;
    }
    audio_driver_dma_buffered(out_queued, out_total);
}

/* ── Stream audio tee ──
 * A PSRAM-backed StreamBuffer (~8 KB, holds ~2 frames of stereo s16).
 * Created on-demand when streaming starts; destroyed when it stops.
 * Audio from the AV task is tee'd here without affecting I2S playback.
 */
#define STREAM_AUDIO_SB_SIZE  (4400 * 2 * sizeof(int16_t) * 2)  /* ~35.2 KB */

esp_err_t av_pipeline_stream_audio_start(void)
{
    if (s_stream_audio_sb) {
        xStreamBufferReset(s_stream_audio_sb);
        ESP_LOGI(TAG, "Stream audio tee reset");
        return ESP_OK;
    }
    s_stream_audio_sb = xStreamBufferCreateWithCaps(
        STREAM_AUDIO_SB_SIZE, 1, MALLOC_CAP_SPIRAM);
    if (!s_stream_audio_sb) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Stream audio tee enabled (%u bytes)", STREAM_AUDIO_SB_SIZE);
    return ESP_OK;
}

void av_pipeline_stream_audio_stop(void)
{
    StreamBufferHandle_t sb = s_stream_audio_sb;
    s_stream_audio_sb = NULL;
    if (sb) {
        /* Give av_output_task one frame period to see the NULL and stop writing */
        vTaskDelay(pdMS_TO_TICKS(20));
        vStreamBufferDeleteWithCaps(sb);
        ESP_LOGI(TAG, "Stream audio tee disabled");
    }
}

size_t av_pipeline_stream_audio_read(int16_t *out, size_t max_bytes)
{
    if (!s_stream_audio_sb) return 0;
    return xStreamBufferReceive(s_stream_audio_sb, out, max_bytes, 0);
}