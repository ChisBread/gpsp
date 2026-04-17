/*
 * gpsp ESP32-P4 — H.264 + audio live streaming over WebSocket
 *
 * Captures GBA frames via av_pipeline, converts RGB565→YUV420 with PPA,
 * encodes to H.264 with hardware encoder, and sends NAL units + PCM audio
 * over a raw WebSocket binary connection.
 *
 * WebSocket binary protocol (server→client, little-endian):
 *   Video frame:
 *     [0]    = 0x01 (video)
 *     [1]    = frame_type (I/P)
 *     [2..5] = uint32 pts (frame counter)
 *     [6..]  = H.264 NAL data
 *
 *   Audio frame:
 *     [0]    = 0x02 (audio)
 *     [1]    = 0x00 (reserved)
 *     [2..5] = uint32 pts
 *     [6..9] = uint32 sample_count (stereo pairs)
 *     [10..] = s16le interleaved stereo PCM
 *
 *   Client→Server commands:
 *     [0] = 0x10  → stop streaming
 *     [0] = 0x20  → reset / start streaming (decoder ready)
 */

#include "av_stream.h"
#include "av_pipeline.h"
#include "common.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <driver/ppa.h>

#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_alloc.h"

static const char *TAG = "av_stream";

/* ---- tunables ---- */
#define STREAM_FPS              30
#define STREAM_BITRATE          256000
#define STREAM_GOP              30
#define STREAM_QP_MIN           18
#define STREAM_QP_MAX           36
#define STREAM_AUDIO_SAMPLES_MAX 4400   /* drain buffer: up to ~4 GBA frames */
#define STREAM_YUV_SIZE         (GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * 3 / 2)

/* ---- module state ---- */
static httpd_handle_t s_server;   /* set by av_stream_set_server() */

static struct {
    volatile bool        active;
    volatile bool        task_done;
    volatile int         ws_fd;         /* stream WS client socket FD (-1 = none) */
    TaskHandle_t         task;
    SemaphoreHandle_t    done_sem;      /* signalled when stream_task exits */

    /* PPA colour converter + H.264 HW encoder (persistent, never freed) */
    ppa_client_handle_t  ppa_client;
    esp_h264_enc_handle_t enc;

    /* Pre-allocated PSRAM buffers */
    uint8_t  *rgb_buf;      /* snapshot from AV pipeline */
    uint8_t  *yuv_buf;      /* PPA output / H264 input   */
    uint8_t  *h264_buf;     /* H264 output NALs           */
    uint8_t  *vid_pkt_buf;  /* WS video frame             */
    uint8_t  *aud_pkt_buf;  /* WS audio frame             */
    int16_t  *audio_buf;    /* PCM audio samples          */
    uint32_t  yuv_actual;
    uint32_t  h264_actual;
    uint32_t  frame_count;
} s_stream;

/* ================================================================
 * Buffer allocation
 * ================================================================ */

static void stream_free_buffers(void)
{
    heap_caps_free(s_stream.rgb_buf);     s_stream.rgb_buf     = NULL;
    heap_caps_free(s_stream.yuv_buf);     s_stream.yuv_buf     = NULL;
    heap_caps_free(s_stream.h264_buf);    s_stream.h264_buf    = NULL;
    heap_caps_free(s_stream.vid_pkt_buf); s_stream.vid_pkt_buf = NULL;
    heap_caps_free(s_stream.aud_pkt_buf); s_stream.aud_pkt_buf = NULL;
    heap_caps_free(s_stream.audio_buf);   s_stream.audio_buf   = NULL;
}

/* Round up to cache-line boundary so DMA never straddles into heap metadata */
#define ALIGN64(x) (((x) + 63u) & ~63u)

static bool stream_alloc_buffers(void)
{
    const uint32_t align     = 64;
    const uint32_t caps      = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const uint32_t rgb_sz    = ALIGN64(GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * 2);
    const uint32_t yuv_sz    = ALIGN64(STREAM_YUV_SIZE);
    const uint32_t h264_sz   = ALIGN64(rgb_sz);              /* 2x for encoder headroom */
    const uint32_t vid_pkt_sz = ALIGN64(h264_sz + 64);
    const uint32_t aud_pkt_sz = ALIGN64(STREAM_AUDIO_SAMPLES_MAX * 2 * sizeof(int16_t) + 64);
    const uint32_t aud_sz    = ALIGN64(STREAM_AUDIO_SAMPLES_MAX * 2 * sizeof(int16_t));

    s_stream.rgb_buf     = heap_caps_aligned_alloc(align, rgb_sz,     caps);
    s_stream.yuv_buf     = heap_caps_aligned_alloc(align, yuv_sz,     caps);
    s_stream.h264_buf    = heap_caps_aligned_alloc(align, h264_sz,    caps);
    s_stream.vid_pkt_buf = heap_caps_aligned_alloc(align, vid_pkt_sz, caps);
    s_stream.aud_pkt_buf = heap_caps_aligned_alloc(align, aud_pkt_sz, caps);
    s_stream.audio_buf   = heap_caps_aligned_alloc(align, aud_sz,     caps);

    s_stream.yuv_actual  = yuv_sz;
    s_stream.h264_actual = h264_sz;

    if (!s_stream.rgb_buf  || !s_stream.yuv_buf  || !s_stream.h264_buf ||
        !s_stream.vid_pkt_buf || !s_stream.aud_pkt_buf || !s_stream.audio_buf) {
        ESP_LOGE(TAG, "Stream buffer alloc failed");
        stream_free_buffers();
        return false;
    }

    ESP_LOGI(TAG, "Stream buffers allocated: %lu B PSRAM",
             (unsigned long)(rgb_sz + yuv_sz + h264_sz + vid_pkt_sz + aud_pkt_sz + aud_sz));
    return true;
}

/* ================================================================
 * PPA + H.264 encoder (created once, kept alive)
 * ================================================================ */

static esp_err_t stream_ensure_encoder(void)
{
    if (!s_stream.ppa_client) {
        ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_SRM };
        esp_err_t err = ppa_register_client(&cfg, &s_stream.ppa_client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "PPA register failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (!s_stream.enc) {
        esp_h264_enc_cfg_hw_t enc_cfg = {
            .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
            .gop      = STREAM_GOP,
            .fps      = STREAM_FPS,
            .res      = { .width = GBA_SCREEN_WIDTH, .height = GBA_SCREEN_HEIGHT },
            .rc       = { .bitrate = STREAM_BITRATE,
                          .qp_min  = STREAM_QP_MIN,
                          .qp_max  = STREAM_QP_MAX },
        };
        esp_h264_err_t ret = esp_h264_enc_hw_new(&enc_cfg, &s_stream.enc);
        if (ret != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "H264 encoder create failed: %d", ret);
            return ESP_FAIL;
        }
        ret = esp_h264_enc_open(s_stream.enc);
        if (ret != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "H264 encoder open failed: %d", ret);
            esp_h264_enc_del(s_stream.enc);
            s_stream.enc = NULL;
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "H264 HW encoder ready (%ux%u, %ubps, GOP=%u)",
                 GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT, STREAM_BITRATE, STREAM_GOP);
    }
    return ESP_OK;
}

/* ================================================================
 * WebSocket send (thread-safe via httpd async API)
 * ================================================================ */

/* Synchronous WS binary send helper. Returns ESP_OK on success. */
static esp_err_t stream_ws_send(uint8_t *payload, size_t len)
{
    if (s_stream.ws_fd < 0 || !s_server) return ESP_FAIL;
    httpd_ws_frame_t ws_frame = {
        .type    = HTTPD_WS_TYPE_BINARY,
        .payload = payload,
        .len     = len,
    };
    esp_err_t err = httpd_ws_send_data(s_server, s_stream.ws_fd, &ws_frame);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Stream WS send failed: %s", esp_err_to_name(err));
        s_stream.ws_fd = -1;
        s_stream.active = false;
    }
    return err;
}

/* ================================================================
 * Streaming task  (runs on the service core)
 * ================================================================ */

static void stream_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Stream task started");

    const uint32_t rgb_frame_size = GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * 2;
    s_stream.frame_count = 0;

    while (s_stream.active && s_stream.ws_fd >= 0) {

        /* 1. Snapshot RGB565 frame (blocks until next GBA VSYNC) */
        if (av_pipeline_snapshot_frame(s_stream.rgb_buf, rgb_frame_size) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(8));
            continue;
        }

        /* 2. Interleave audio / video on alternating frames.
         *    Even → video (H.264),  Odd → audio (PCM). */

        /* --- ODD: send accumulated audio --- */
        if ((s_stream.frame_count & 1) != 0 && s_stream.ws_fd >= 0) {
            if (av_pipeline_audio_enabled()) {
                const uint32_t max_bytes = STREAM_AUDIO_SAMPLES_MAX * 2 * sizeof(int16_t);
                size_t bytes   = av_pipeline_stream_audio_read(s_stream.audio_buf, max_bytes);
                uint32_t samples = bytes / (2 * sizeof(int16_t));
                if (samples > 0) {
                    uint8_t *pkt = s_stream.aud_pkt_buf;
                    pkt[0] = 0x02;
                    pkt[1] = 0x00;
                    uint32_t pts = s_stream.frame_count;
                    memcpy(&pkt[2], &pts, 4);
                    memcpy(&pkt[6], &samples, 4);
                    uint32_t pcm_bytes = samples * 2 * sizeof(int16_t);
                    memcpy(&pkt[10], s_stream.audio_buf, pcm_bytes);
                    stream_ws_send(pkt, 10 + pcm_bytes);
                }
            }
        }

        /* --- EVEN: encode + send video --- */
        if ((s_stream.frame_count & 1) == 0 && s_stream.ws_fd >= 0) {
            /* RGB565 → YUV420 via PPA */
            ppa_srm_oper_config_t srm_cfg = {
                .in = {
                    .buffer          = s_stream.rgb_buf,
                    .pic_w           = GBA_SCREEN_WIDTH,
                    .pic_h           = GBA_SCREEN_HEIGHT,
                    .block_w         = GBA_SCREEN_WIDTH,
                    .block_h         = GBA_SCREEN_HEIGHT,
                    .block_offset_x  = 0,
                    .block_offset_y  = 0,
                    .srm_cm          = PPA_SRM_COLOR_MODE_RGB565,
                },
                .out = {
                    .buffer          = s_stream.yuv_buf,
                    .buffer_size     = s_stream.yuv_actual,
                    .pic_w           = GBA_SCREEN_WIDTH,
                    .pic_h           = GBA_SCREEN_HEIGHT,
                    .block_offset_x  = 0,
                    .block_offset_y  = 0,
                    .srm_cm          = PPA_SRM_COLOR_MODE_YUV420,
                },
                .rotation_angle  = PPA_SRM_ROTATION_ANGLE_0,
                .scale_x         = 1.0f,
                .scale_y         = 1.0f,
                .rgb_swap        = false,
                .byte_swap       = false,
                .mode            = PPA_TRANS_MODE_BLOCKING,
            };
            if (ppa_do_scale_rotate_mirror(s_stream.ppa_client, &srm_cfg) != ESP_OK) {
                s_stream.frame_count++;
                continue;
            }

            /* YUV420 → H.264 */
            esp_h264_enc_in_frame_t in_frame = {
                .raw_data = { .buffer = s_stream.yuv_buf, .len = s_stream.yuv_actual },
                .pts = s_stream.frame_count,
            };
            esp_h264_enc_out_frame_t out_frame = {
                .raw_data = { .buffer = s_stream.h264_buf, .len = s_stream.h264_actual },
            };
            esp_h264_err_t h264_ret = esp_h264_enc_process(s_stream.enc, &in_frame, &out_frame);
            if (h264_ret != ESP_H264_ERR_OK) {
                ESP_LOGW(TAG, "H264 encode failed: %d (frame %lu)",
                         h264_ret, (unsigned long)s_stream.frame_count);
                s_stream.frame_count++;
                continue;
            }

            if (out_frame.length > s_stream.h264_actual) {
                ESP_LOGE(TAG, "H264 output overflow: %u > %lu, stopping!",
                         (unsigned)out_frame.length, (unsigned long)s_stream.h264_actual);
                break;
            }

            if (s_stream.ws_fd >= 0 && out_frame.length > 0) {
                uint8_t *pkt = s_stream.vid_pkt_buf;
                pkt[0] = 0x01;
                pkt[1] = (uint8_t)out_frame.frame_type;
                uint32_t pts = s_stream.frame_count;
                memcpy(&pkt[2], &pts, 4);
                memcpy(&pkt[6], out_frame.raw_data.buffer, out_frame.length);
                stream_ws_send(pkt, 6 + out_frame.length);
            }
        }

        s_stream.frame_count++;
    }

    ESP_LOGI(TAG, "Stream task ending");
    av_pipeline_stream_audio_stop();
    s_stream.active = false;
    s_stream.task_done = true;
    xSemaphoreGive(s_stream.done_sem);   /* wake whoever is waiting */
    vTaskSuspend(NULL);  /* let handler reap us via vTaskDeleteWithCaps */
}

/* ================================================================
 * Start / stop helpers
 * ================================================================ */

/* Reap a finished stream task (free its WithCaps stack+TCB). */
static void stream_reap_task(void)
{
    if (s_stream.task && s_stream.task_done) {
        vTaskDeleteWithCaps(s_stream.task);
        s_stream.task = NULL;
        s_stream.task_done = false;
        ESP_LOGI(TAG, "Reaped stream task");
    }
}

/* Stop the stream task and wait until it has fully exited.
 * Only then is it safe to free buffers or reap the task. */
static void stream_stop_and_wait(void)
{
    if (!s_stream.task || s_stream.task_done) return;
    s_stream.active = false;
    /* Task may block up to 100 ms in snapshot + PPA + H264, give 500 ms. */
    if (xSemaphoreTake(s_stream.done_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "Stream task did not exit in 500 ms, force-deleting");
        vTaskDeleteWithCaps(s_stream.task);
        s_stream.task = NULL;
        s_stream.task_done = false;
        av_pipeline_stream_audio_stop();
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

void av_stream_set_server(httpd_handle_t server)
{
    s_server = server;
    if (!s_stream.done_sem) {
        s_stream.done_sem = xSemaphoreCreateBinary();
    }
}

esp_err_t av_stream_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "Stream WS client connected, fd=%d", fd);
        s_stream.ws_fd = fd;
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = {0};
    uint8_t buf[8];
    ws_pkt.payload = buf;
    ws_pkt.type    = HTTPD_WS_TYPE_BINARY;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf));
    if (ret != ESP_OK) {
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "Stream WS recv failed on fd=%d", fd);
        if (s_stream.ws_fd == fd) {
            s_stream.ws_fd = -1;
            stream_stop_and_wait();
            stream_reap_task();
            stream_free_buffers();
        }
        return ESP_FAIL;
    }

    /* 0x10 — stop streaming */
    if (ws_pkt.len >= 1 && buf[0] == 0x10) {
        int fd = httpd_req_to_sockfd(req);
        if (s_stream.ws_fd == fd) {
            ESP_LOGI(TAG, "Stream stop requested by client");
            s_stream.ws_fd = -1;
            stream_stop_and_wait();
            stream_reap_task();
            stream_free_buffers();
        }
    }

    /* 0x20 — reset / start streaming (client decoder ready) */
    if (ws_pkt.len >= 1 && buf[0] == 0x20) {
        int fd = httpd_req_to_sockfd(req);
        if (s_stream.ws_fd != fd) {
            ESP_LOGW(TAG, "Ignoring reset from stale fd=%d (current=%d)", fd, s_stream.ws_fd);
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Stream reset requested by client (fd=%d)", fd);

        /* Tear down any previous session */
        stream_stop_and_wait();
        stream_reap_task();
        stream_free_buffers();

        if (!stream_alloc_buffers())
            return ESP_OK;
        if (stream_ensure_encoder() != ESP_OK) {
            stream_free_buffers();
            return ESP_OK;
        }

        /* Reset encoder so first frame is IDR */
        esp_h264_enc_close(s_stream.enc);
        if (esp_h264_enc_open(s_stream.enc) != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "H264 encoder reopen failed");
            stream_free_buffers();
            return ESP_OK;
        }
        av_pipeline_stream_audio_start();

        s_stream.active      = true;
        s_stream.frame_count = 0;

        BaseType_t core = (CONFIG_GPSP_EMULATION_CORE == 0) ? 1 : 0;
        BaseType_t xret = xTaskCreatePinnedToCoreWithCaps(
            stream_task, "av_stream",
            8192, NULL,
            tskIDLE_PRIORITY + 3,
            &s_stream.task, core,
            MALLOC_CAP_SPIRAM);
        if (xret != pdPASS) {
            ESP_LOGE(TAG, "Stream task creation failed");
            av_pipeline_stream_audio_stop();
            stream_free_buffers();
            s_stream.active = false;
        }
    }

    return ESP_OK;
}
