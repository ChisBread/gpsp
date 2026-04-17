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
#define STREAM_BITRATE          384000
#define STREAM_GOP              30
#define STREAM_QP_MIN           18
#define STREAM_QP_MAX           28
#define STREAM_AUDIO_SAMPLES_MAX 4400   /* drain buffer: up to ~4 GBA frames */

/* ---- module state ---- */
static httpd_handle_t s_server;   /* set by av_stream_set_server() */

static struct {
    volatile bool        active;
    volatile int         ws_fd;         /* stream WS client socket FD (-1 = none) */
    volatile int         pending_fd;    /* handler→task: start session with this fd, -1 = none */
    TaskHandle_t         task;

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
 *
 * Only called from stream_task — no concurrent access.
 * httpd_ws_send_data() is synchronous (blocks until socket send
 * completes), so buffers are safe to free after the task loop exits.
 * ================================================================ */

/* Round up to cache-line boundary so DMA never straddles into heap metadata */
#define ALIGN64(x) (((x) + 63u) & ~63u)

static void stream_free_buffers(void)
{
    heap_caps_free(s_stream.rgb_buf);     s_stream.rgb_buf     = NULL;
    heap_caps_free(s_stream.yuv_buf);     s_stream.yuv_buf     = NULL;
    heap_caps_free(s_stream.h264_buf);    s_stream.h264_buf    = NULL;
    heap_caps_free(s_stream.vid_pkt_buf); s_stream.vid_pkt_buf = NULL;
    heap_caps_free(s_stream.aud_pkt_buf); s_stream.aud_pkt_buf = NULL;
    heap_caps_free(s_stream.audio_buf);   s_stream.audio_buf   = NULL;
}

static bool stream_alloc_buffers(void)
{
    if (s_stream.rgb_buf) return true;   /* already allocated */

    const uint32_t align     = 64;
    const uint32_t caps      = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const uint32_t rgb_sz    = ALIGN64(GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * 2);
    const uint32_t yuv_sz    = ALIGN64(GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * 3 / 2);
    const uint32_t h264_sz   = ALIGN64(rgb_sz);              /* generous encoder headroom */
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

        /* Bypass deblocking filter — preserves sharp pixel-art edges.
         * db_bypass is ctrl[0] offset 0x18 in h264_ctrl_regs_t,
         * ctrl[0] starts at h264_dev_t + 0x08.  Base = 0x50084000. */
        volatile uint32_t *db_bypass = (volatile uint32_t *)0x50084020;
        *db_bypass = 1;
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
 * Streaming task  (persistent — runs forever, sleeps between sessions)
 *
 * The handler NEVER blocks — it only sets pending_fd and notifies.
 * All resource management (buf acquire/release, audio start/stop,
 * httpd flush) happens here, avoiding the deadlock where the handler
 * blocks the httpd thread while the task tries to send via httpd.
 * ================================================================ */

static void stream_task(void *arg)
{
    (void)arg;
    const uint32_t rgb_frame_size = GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * 2;

    for (;;) {
        /* Sleep until the handler posts a new session. */
        if (s_stream.pending_fd < 0) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }

        int new_fd = s_stream.pending_fd;
        s_stream.pending_fd = -1;
        if (new_fd < 0) continue;

        /* ── Set up session ── */
        if (!stream_alloc_buffers()) continue;
        if (stream_ensure_encoder() != ESP_OK) {
            stream_free_buffers();
            continue;
        }
        esp_h264_enc_close(s_stream.enc);
        if (esp_h264_enc_open(s_stream.enc) != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "H264 encoder reopen failed");
            stream_free_buffers();
            continue;
        }
        av_pipeline_stream_audio_start();

        s_stream.ws_fd       = new_fd;
        s_stream.active      = true;
        s_stream.frame_count = 0;

        ESP_LOGI(TAG, "Stream session started (fd=%d)", new_fd);
        const TickType_t frame_period = pdMS_TO_TICKS(1000 / STREAM_FPS);
        TickType_t last_tick = xTaskGetTickCount();

        while (s_stream.active && s_stream.ws_fd >= 0) {

            /* Break early if handler posted a new session */
            if (s_stream.pending_fd >= 0) break;

            /* ── Audio: drain accumulated PCM ── */
            if (av_pipeline_audio_enabled() && s_stream.ws_fd >= 0) {
                const uint32_t max_bytes = STREAM_AUDIO_SAMPLES_MAX * 2 * sizeof(int16_t);
                size_t bytes = av_pipeline_stream_audio_read(s_stream.audio_buf, max_bytes);
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

            /* ── Video: snapshot → PPA → H264 → send ── */
            if (av_pipeline_snapshot_frame(s_stream.rgb_buf, rgb_frame_size) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(8));
                continue;
            }

            if (s_stream.ws_fd < 0) break;

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

            s_stream.frame_count++;

            /* ── Rate limit to STREAM_FPS ── */
            vTaskDelayUntil(&last_tick, frame_period);
        }

        /* ── Session cleanup (runs in task context, httpd is NOT blocked) ── */
        ESP_LOGI(TAG, "Stream session ending");
        s_stream.active = false;
        av_pipeline_stream_audio_stop();
        stream_free_buffers();
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

void av_stream_set_server(httpd_handle_t server)
{
    s_server = server;
    s_stream.pending_fd = -1;
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
            s_stream.ws_fd = -1;   /* task will see this and exit */
        }
        return ESP_FAIL;
    }

    /* 0x10 — stop streaming (handler never blocks) */
    if (ws_pkt.len >= 1 && buf[0] == 0x10) {
        int fd = httpd_req_to_sockfd(req);
        if (s_stream.ws_fd == fd) {
            ESP_LOGI(TAG, "Stream stop requested by client");
            s_stream.ws_fd = -1;   /* task will see this and exit */
        }
    }

    /* 0x20 — reset / start streaming (client decoder ready)
     * Handler only sets pending_fd and notifies the task.
     * The task handles all resource management. */
    if (ws_pkt.len >= 1 && buf[0] == 0x20) {
        int fd = httpd_req_to_sockfd(req);
        if (s_stream.ws_fd != fd) {
            ESP_LOGW(TAG, "Ignoring reset from stale fd=%d (current=%d)", fd, s_stream.ws_fd);
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Stream reset requested by client (fd=%d)", fd);

        /* Signal current session to stop */
        s_stream.ws_fd = -1;

        /* Lazy one-time task creation */
        if (!s_stream.task) {
            BaseType_t core = (CONFIG_GPSP_EMULATION_CORE == 0) ? 1 : 0;
            BaseType_t xret = xTaskCreatePinnedToCoreWithCaps(
                stream_task, "av_stream",
                16384, NULL,
                tskIDLE_PRIORITY + 3,
                &s_stream.task, core,
                MALLOC_CAP_SPIRAM);
            if (xret != pdPASS) {
                ESP_LOGE(TAG, "Stream task creation failed");
                return ESP_OK;
            }
        }

        /* Post new session — task picks it up, sets up encoder, etc. */
        s_stream.pending_fd = fd;
        xTaskNotifyGive(s_stream.task);
    }

    return ESP_OK;
}
