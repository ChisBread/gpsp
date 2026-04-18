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
#include "esp_cache.h"

static const char *TAG = "av_stream";

/* ---- tunables ---- */
#define STREAM_FPS              30
#define STREAM_BITRATE          2000000
#define STREAM_GOP              30
#define STREAM_QP_MIN           18
#define STREAM_QP_MAX           26
#define STREAM_AUDIO_SAMPLES_MAX 4400   /* drain buffer: up to ~4 GBA frames */

/* ================================================================
 * IMA-ADPCM encoder  (4:1 compression, ~256kbps stereo @ 32768Hz)
 * ================================================================ */

static const int16_t ima_step_table[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
    50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,
    253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
    1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,
    3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
    10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
    27086,29794,32767
};

static const int8_t ima_index_table[16] = {
    -1,-1,-1,-1, 2,4,6,8,
    -1,-1,-1,-1, 2,4,6,8
};

typedef struct {
    int16_t predictor;
    uint8_t step_index;
} adpcm_state_t;

static uint8_t adpcm_encode_sample(adpcm_state_t *st, int16_t sample)
{
    int step = ima_step_table[st->step_index];
    int diff = sample - st->predictor;
    uint8_t nibble = 0;
    if (diff < 0) { nibble = 8; diff = -diff; }
    if (diff >= step)     { nibble |= 4; diff -= step; }
    if (diff >= step / 2) { nibble |= 2; diff -= step / 2; }
    if (diff >= step / 4) { nibble |= 1; }
    /* Decode in-place to track predictor exactly like decoder */
    int pred = st->predictor;
    int delta = step >> 3;
    if (nibble & 4) delta += step;
    if (nibble & 2) delta += step >> 1;
    if (nibble & 1) delta += step >> 2;
    if (nibble & 8) pred -= delta; else pred += delta;
    if (pred > 32767)  pred = 32767;
    if (pred < -32768) pred = -32768;
    st->predictor = (int16_t)pred;
    int idx = st->step_index + ima_index_table[nibble];
    if (idx < 0)  idx = 0;
    if (idx > 88) idx = 88;
    st->step_index = (uint8_t)idx;
    return nibble;
}

static adpcm_state_t s_adpcm_l, s_adpcm_r;

/* ================================================================
 * Software RGB565 → O_UYY_E_VYY (BT.601 limited range)
 *
 * Replaces PPA hardware conversion.  For 240×160 this takes ~1 ms
 * on the ESP32-P4 — well within the 33 ms frame budget at 30 fps.
 *
 * O_UYY_E_VYY layout (Espressif packed YUV420):
 *   even rows (0,2,4…): U Y Y U Y Y …   (U + 2 luma)
 *   odd  rows (1,3,5…): V Y Y V Y Y …   (V + 2 luma)
 * Each triplet covers 2 horizontal pixels; U/V is shared between
 * the 2×2 block formed by (even_row, odd_row) × (col, col+1).
 * ================================================================ */
static void sw_rgb565_to_ouyy_evyy(const uint16_t *rgb, uint8_t *yuv,
                                    uint32_t width, uint32_t height)
{
    const uint32_t row_bytes = width * 3 / 2;  /* 360 for w=240 */

    for (uint32_t y = 0; y < height; y += 2) {
        const uint16_t *row0 = rgb + y * width;
        const uint16_t *row1 = rgb + (y + 1) * width;
        uint8_t *dst_u = yuv + y * row_bytes;           /* U row */
        uint8_t *dst_v = yuv + (y + 1) * row_bytes;     /* V row */

        for (uint32_t x = 0; x < width; x += 2) {
            uint16_t p00 = row0[x],   p10 = row0[x + 1];
            uint16_t p01 = row1[x],   p11 = row1[x + 1];

            /* RGB565 → 8-bit per channel (full-scale expansion) */
            #define R8(c) (uint8_t)(((c) >> 8) & 0xF8)  /* top 5 bits → 8 */
            #define G8(c) (uint8_t)(((c) >> 3) & 0xFC)  /* mid 6 bits → 8 */
            #define B8(c) (uint8_t)(((c) << 3) & 0xF8)  /* low 5 bits → 8 */

            int r00 = R8(p00), g00 = G8(p00), b00 = B8(p00);
            int r10 = R8(p10), g10 = G8(p10), b10 = B8(p10);
            int r01 = R8(p01), g01 = G8(p01), b01 = B8(p01);
            int r11 = R8(p11), g11 = G8(p11), b11 = B8(p11);

            #undef R8
            #undef G8
            #undef B8

            /* BT.601 limited range luma:
             * Y = ((66*R + 129*G + 25*B + 128) >> 8) + 16  */
            uint8_t y00 = ((66 * r00 + 129 * g00 + 25 * b00 + 128) >> 8) + 16;
            uint8_t y10 = ((66 * r10 + 129 * g10 + 25 * b10 + 128) >> 8) + 16;
            uint8_t y01 = ((66 * r01 + 129 * g01 + 25 * b01 + 128) >> 8) + 16;
            uint8_t y11 = ((66 * r11 + 129 * g11 + 25 * b11 + 128) >> 8) + 16;

            /* Chroma: average 2×2 block */
            int ra = (r00 + r10 + r01 + r11 + 2) >> 2;
            int ga = (g00 + g10 + g01 + g11 + 2) >> 2;
            int ba = (b00 + b10 + b01 + b11 + 2) >> 2;

            uint8_t u = ((-38 * ra - 74 * ga + 112 * ba + 128) >> 8) + 128;
            uint8_t v = ((112 * ra - 94 * ga - 18 * ba + 128) >> 8) + 128;

            /* Write triplets */
            uint32_t tri = (x / 2) * 3;
            dst_u[tri]     = u;
            dst_u[tri + 1] = y00;
            dst_u[tri + 2] = y10;
            dst_v[tri]     = v;
            dst_v[tri + 1] = y01;
            dst_v[tri + 2] = y11;
        }
    }
}

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
    /* IMA-ADPCM: 1 byte per 2 samples (4-bit nibbles), stereo interleaved
     * + 18 bytes header. Max ADPCM payload = samples * 2 / 2 = samples */
    const uint32_t aud_pkt_sz = ALIGN64(STREAM_AUDIO_SAMPLES_MAX + 64);
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

    /* Fill YUV buffer with neutral value (Y=128, U=128, V=128 = mid-gray).
     * Uninitialized PSRAM often contains 0xFF which decodes to bright magenta
     * in O_UYY_E_VYY → BT.601 conversion.  This prevents flash-purple if PPA
     * ever produces a partial frame. */
    memset(s_stream.yuv_buf, 0x80, yuv_sz);
    memset(s_stream.rgb_buf, 0, rgb_sz);

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
        s_adpcm_l = (adpcm_state_t){0, 0};
        s_adpcm_r = (adpcm_state_t){0, 0};

        ESP_LOGI(TAG, "Stream session started (fd=%d)", new_fd);
        const TickType_t frame_period = pdMS_TO_TICKS(1000 / STREAM_FPS);
        TickType_t last_tick = xTaskGetTickCount();

        while (s_stream.active && s_stream.ws_fd >= 0) {

            /* Break early if handler posted a new session */
            if (s_stream.pending_fd >= 0) break;

            /* ── Audio: drain accumulated PCM, encode to IMA-ADPCM ── */
            if (av_pipeline_audio_enabled() && s_stream.ws_fd >= 0) {
                const uint32_t max_bytes = STREAM_AUDIO_SAMPLES_MAX * 2 * sizeof(int16_t);
                size_t bytes = av_pipeline_stream_audio_read(s_stream.audio_buf, max_bytes);
                uint32_t samples = bytes / (2 * sizeof(int16_t));
                if (samples > 0) {
                    uint8_t *pkt = s_stream.aud_pkt_buf;
                    pkt[0] = 0x02;
                    pkt[1] = 0x01;  /* ADPCM sub-type */
                    uint32_t pts = s_stream.frame_count;
                    memcpy(&pkt[2], &pts, 4);
                    memcpy(&pkt[6], &samples, 4);
                    /* ADPCM state headers (predictor + step_index per channel) */
                    memcpy(&pkt[10], &s_adpcm_l.predictor, 2);
                    pkt[12] = s_adpcm_l.step_index;
                    pkt[13] = 0; /* pad */
                    memcpy(&pkt[14], &s_adpcm_r.predictor, 2);
                    pkt[16] = s_adpcm_r.step_index;
                    pkt[17] = 0; /* pad */
                    /* Encode interleaved stereo → packed nibbles */
                    const int16_t *pcm = s_stream.audio_buf;
                    uint8_t *dst = &pkt[18];
                    for (uint32_t i = 0; i < samples; i++) {
                        uint8_t nL = adpcm_encode_sample(&s_adpcm_l, pcm[i * 2]);
                        uint8_t nR = adpcm_encode_sample(&s_adpcm_r, pcm[i * 2 + 1]);
                        dst[i] = (nL & 0x0F) | (nR << 4);
                    }
                    stream_ws_send(pkt, 18 + samples);
                }
            }

            /* ── Video: snapshot → YUV → H264 → send ── */
            if (av_pipeline_snapshot_frame(s_stream.rgb_buf, rgb_frame_size) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(8));
                continue;
            }

            if (s_stream.ws_fd < 0) break;

            /* RGB565 → O_UYY_E_VYY (software, BT.601 limited range).
             * Replaces PPA hardware path to rule out PPA DMA/cache bugs. */
            sw_rgb565_to_ouyy_evyy((const uint16_t *)s_stream.rgb_buf,
                                   s_stream.yuv_buf,
                                   GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT);

            /* Flush CPU-written YUV data to PSRAM so H264 DMA sees it */
            esp_cache_msync(s_stream.yuv_buf, s_stream.yuv_actual,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M);

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
