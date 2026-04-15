/*
 * gpsp ESP32-P4 — Embedded HTTP/WebSocket server
 *
 * Currently provides web-based GBA debug input with latency tracking.
 * Designed as a generic server for future OTA, data transfer, etc.
 *
 * WebSocket binary protocol (all little-endian):
 *   Client→Server:
 *     [0]    = 0x01 (button update)
 *     [1..2] = uint16 keys bitmask (GBA_KEY_* bits)
 *     [3..6] = uint32 client timestamp ms (for RTT calc)
 *
 *   Server→Client:
 *     [0]    = 0x81 (button ack)
 *     [1..4] = uint32 echoed client timestamp ms
 *     [5..8] = uint32 emu_latency_us (time from WS recv to emu read)
 *
 *   Client→Server:
 *     [0]    = 0x02 (ping)
 *     [1..4] = uint32 client timestamp ms
 *
 *   Server→Client:
 *     [0]    = 0x82 (pong)
 *     [1..4] = uint32 echoed client timestamp ms
 */

#include "web_server.h"
#include "runtime_config.h"
#include "gba_session.h"
#include "storage.h"
#include "esp32p4/input_driver.h"
#include "c6_remote.h"

#include <string.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <driver/ppa.h>

#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_alloc.h"

#include "av_pipeline.h"
#include "common.h"
#include "cpu.h"
#include "main.h"
#include "savestate.h"

static const char *TAG = "web_server";

/* ---- state ---- */
static httpd_handle_t s_server;
static volatile uint16_t s_web_keys_held;     /* last WS state */
static volatile uint16_t s_web_keys_pressed;  /* OR-accumulated presses since last emu read */
static volatile int64_t  s_last_recv_us;      /* timestamp of last WS recv */
static volatile int64_t  s_last_emu_read_us;  /* timestamp of last emu read */

/* ---- frame capture mutex (serialises /api/frame requests) ---- */
static SemaphoreHandle_t s_frame_mutex;

/* ---- H.264 live streaming state ---- */
#define STREAM_FPS          30    /* target stream FPS (every other GBA frame) */
#define STREAM_BITRATE      512000
#define STREAM_GOP          30
#define STREAM_QP_MIN       18
#define STREAM_QP_MAX       36
#define STREAM_AUDIO_SAMPLES_MAX  4400  /* drain buffer may hold up to ~4 GBA frames */
#define STREAM_YUV_SIZE     (GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * 3 / 2)  /* 57600 */

static struct {
    volatile bool        active;       /* streaming loop running? */
    volatile bool        task_done;    /* task finished, awaiting reap */
    volatile int         ws_fd;        /* stream WS client socket FD (-1 = none) */
    TaskHandle_t         task;         /* streaming task handle */
    /* PPA for RGB565→YUV420 color conversion */
    ppa_client_handle_t  ppa_client;
    /* H.264 HW encoder */
    esp_h264_enc_handle_t enc;
    /* Pre-allocated buffers (PSRAM, allocated once) */
    uint8_t *rgb_buf;     /* 76800 B — snapshot from AV pipeline */
    uint8_t *yuv_buf;     /* 57600 B — PPA output, H264 input */
    uint8_t *h264_buf;    /* 57600 B — H264 output NALs */
    uint8_t *vid_pkt_buf; /* 57600+16 B — video WS frame */
    uint8_t *aud_pkt_buf; /* ~17600+16 B — audio WS frame */
    int16_t *audio_buf;   /* ~4400 × 2 B — audio samples */
    uint32_t yuv_actual;  /* actual allocated size (may be rounded for cache) */
    uint32_t h264_actual;
    uint32_t frame_count;
} s_stream;

/* ---- embedded HTML ---- */
extern const char web_home_html_start[] asm("_binary_web_home_html_start");
extern const char web_home_html_end[]   asm("_binary_web_home_html_end");
extern const char web_server_html_start[] asm("_binary_web_server_html_start");
extern const char web_server_html_end[]   asm("_binary_web_server_html_end");

/* ---- HTTP GET / → home page ---- */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    size_t len = web_home_html_end - web_home_html_start;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, web_home_html_start, len);
}

/* ---- HTTP GET /input → input controller page ---- */
static esp_err_t input_get_handler(httpd_req_t *req)
{
    size_t len = web_server_html_end - web_server_html_start;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, web_server_html_start, len);
}

/* ---- WebSocket handler ---- */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WS client connected");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = {0};
    uint8_t buf[16];
    ws_pkt.payload = buf;
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS recv error: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ws_pkt.len < 1) return ESP_OK;

    int64_t now_us = esp_timer_get_time();

    if (buf[0] == 0x01 && ws_pkt.len >= 7) {
        /* Button update */
        uint16_t keys = (uint16_t)(buf[1] | (buf[2] << 8)) & 0x3FF;
        uint32_t client_ts = buf[3] | (buf[4] << 8) | (buf[5] << 16) | (buf[6] << 24);

        /* Detect new presses vs previous held state and accumulate */
        uint16_t new_presses = keys & ~s_web_keys_held;
        s_web_keys_pressed |= new_presses;
        s_web_keys_held = keys;
        s_last_recv_us = now_us;

        /* Immediately update GBA P1 register so mid-frame reads see
         * the new keys without waiting for next frame boundary.
         * Merge with physical GPIO buttons to avoid losing them. */
        uint16_t combined = keys | input_driver_read();
        write_ioreg(REG_P1, (~combined) & 0x3FF);

        /* Send ACK with echoed timestamp */
        uint8_t ack[9];
        ack[0] = 0x81;
        memcpy(&ack[1], &client_ts, 4);
        /* emu latency will be approximate: last measured frame interval */
        uint32_t emu_lat = 0;
        if (s_last_emu_read_us > 0) {
            int64_t delta = now_us - s_last_emu_read_us;
            if (delta > 0 && delta < 1000000) emu_lat = (uint32_t)delta;
        }
        memcpy(&ack[5], &emu_lat, 4);

        httpd_ws_frame_t ack_pkt = {
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = ack,
            .len = sizeof(ack),
        };
        httpd_ws_send_frame(req, &ack_pkt);

    } else if (buf[0] == 0x02 && ws_pkt.len >= 5) {
        /* Ping → Pong */
        uint8_t pong[5];
        pong[0] = 0x82;
        memcpy(&pong[1], &buf[1], 4);
        httpd_ws_frame_t pong_pkt = {
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = pong,
            .len = sizeof(pong),
        };
        httpd_ws_send_frame(req, &pong_pkt);
    }

    return ESP_OK;
}

/* ---- GET /api/stats → JSON performance snapshot ---- */
static esp_err_t stats_get_handler(httpd_req_t *req)
{
    char buf[2048];
    int len = gba_session_stats_json(buf, sizeof(buf));
    if (len < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stats unavailable");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, len);
}

/* ---- GET /api/settings → current settings JSON ---- */
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    char buf[384];
    int len = snprintf(buf, sizeof(buf),
        "{\"dynarec_enable\":%d,\"sprite_limit\":%d,\"boot_mode\":\"%s\""
        ",\"frameskip_type\":%u,\"frameskip_interval\":%u,\"frameskip_threshold\":%u}",
        dynarec_enable ? 1 : 0,
        sprite_limit ? 1 : 0,
        selected_boot_mode == boot_bios ? "bios" : "game",
        (unsigned)gpsp_frameskip_type,
        (unsigned)gpsp_frameskip_interval,
        (unsigned)gpsp_frameskip_threshold);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, len);
}

/* ---- POST /api/settings → apply + save ---- */
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    /* Minimal JSON number extraction — no external JSON lib needed */
    char *p;

    p = strstr(body, "\"dynarec_enable\"");
    if (p) {
        p = strchr(p + 16, ':');
        if (p) {
            int v = atoi(p + 1);
#ifdef HAVE_DYNAREC
            if (dynarec_enable != v) {
                dynarec_enable = v;
                flush_dynarec_caches();
            }
#else
            (void)v;
            dynarec_enable = 0;
#endif
        }
    }

    p = strstr(body, "\"sprite_limit\"");
    if (p) {
        p = strchr(p + 14, ':');
        if (p) sprite_limit = atoi(p + 1) ? 1 : 0;
    }

    p = strstr(body, "\"boot_mode\"");
    if (p) {
        if (strstr(p, "\"bios\""))
            selected_boot_mode = boot_bios;
        else
            selected_boot_mode = boot_game;
    }

    p = strstr(body, "\"frameskip_type\"");
    if (p) {
        p = strchr(p + 16, ':');
        if (p) {
            int v = atoi(p + 1);
            if (v >= 0 && v <= 3)
                gpsp_frameskip_type = (uint32_t)v;
        }
    }

    p = strstr(body, "\"frameskip_interval\"");
    if (p) {
        p = strchr(p + 20, ':');
        if (p) {
            int v = atoi(p + 1);
            if (v >= 0 && v <= 9)
                gpsp_frameskip_interval = (uint32_t)v;
        }
    }

    p = strstr(body, "\"frameskip_threshold\"");
    if (p) {
        p = strchr(p + 21, ':');
        if (p) {
            int v = atoi(p + 1);
            if (v >= 0 && v <= 100)
                gpsp_frameskip_threshold = (uint32_t)v;
        }
    }

    gpsp_runtime_config_save();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

/* ---- GET /api/roms → list .gba files on SD ---- */
static esp_err_t roms_get_handler(httpd_req_t *req)
{
    char **entries = NULL;
    size_t count = 0;

    esp_err_t err = storage_list_roms(NULL, &entries, &count);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD read failed");
        return ESP_FAIL;
    }

    /* Build JSON array */
    char buf[2048];
    char *p = buf;
    char *end = buf + sizeof(buf) - 1;
    *p++ = '[';
    for (size_t i = 0; i < count && p < end - 4; i++) {
        if (i > 0) *p++ = ',';
        int n = snprintf(p, end - p, "\"%s\"", entries[i]);
        if (n > 0 && p + n < end) p += n;
    }
    *p++ = ']';
    *p = '\0';

    storage_free_rom_list(entries, count);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, (ssize_t)(p - buf));
}

/* ---- POST /api/roms/load → switch to a different ROM ---- */
static esp_err_t roms_load_handler(httpd_req_t *req)
{
    char body[600];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    /* Extract "file":"xxx.gba" */
    char *p = strstr(body, "\"file\"");
    if (!p) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing file field");
        return ESP_FAIL;
    }
    p = strchr(p + 6, '"');
    if (!p) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    p++; /* skip opening quote */
    char *q = strchr(p, '"');
    if (!q || q - p < 1 || (size_t)(q - p) >= sizeof(body) - 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad filename");
        return ESP_FAIL;
    }
    *q = '\0';

    /* Validate: must end in .gba, no path separators (prevent traversal) */
    size_t flen = strlen(p);
    if (flen < 5 || strcasecmp(&p[flen - 4], ".gba") != 0 ||
        strchr(p, '/') || strchr(p, '\\') || strstr(p, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid filename");
        return ESP_FAIL;
    }

    /* Build full path */
    char rom_path[512];
    snprintf(rom_path, sizeof(rom_path), "%s/%s", STORAGE_MOUNT_POINT, p);

    ESP_LOGI(TAG, "ROM switch requested: %s", rom_path);

    gba_session_reload_request_t reload = {
        .rom_path = rom_path,
        .bios_path = NULL,
        .reload_rom = true,
        .reload_bios = false,
        .clear_backup = true,
    };
    esp_err_t err = gba_session_request_reload(&reload);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (err == ESP_OK) {
        return httpd_resp_send(req, "{\"ok\":true}", 11);
    } else {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"reload failed\"}", 36);
    }
}

/* ---- GET /api/states → list savestate slots for current ROM ---- */
#define STATE_MAX_SLOTS 10

static esp_err_t states_get_handler(httpd_req_t *req)
{
    const char *rom_path = gba_session_current_rom_path();
    if (!rom_path) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, "{\"slots\":[]}", 12);
    }

    unsigned slots[STATE_MAX_SLOTS];
    size_t slot_sizes[STATE_MAX_SLOTS];
    size_t count = 0;

    esp_err_t err = storage_list_states(rom_path, slots, slot_sizes, STATE_MAX_SLOTS, &count);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "state list failed");
        return ESP_FAIL;
    }

    char buf[512];
    char *p = buf;
    char *end = buf + sizeof(buf) - 1;
    int n = snprintf(p, end - p, "{\"slots\":[");
    p += n;
    for (size_t i = 0; i < count && p < end - 32; i++) {
        if (i > 0) *p++ = ',';
        n = snprintf(p, end - p, "{\"slot\":%u,\"size\":%u}", slots[i], (unsigned)slot_sizes[i]);
        if (n > 0) p += n;
    }
    n = snprintf(p, end - p, "]}");
    p += n;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, (ssize_t)(p - buf));
}

/* ---- POST /api/states/save → save state to slot ---- */
static esp_err_t states_save_handler(httpd_req_t *req)
{
    char body[64];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char *p = strstr(body, "\"slot\"");
    if (!p) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing slot");
        return ESP_FAIL;
    }
    p = strchr(p + 6, ':');
    if (!p) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    unsigned slot = (unsigned)atoi(p + 1);
    if (slot >= STATE_MAX_SLOTS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
        return ESP_FAIL;
    }

    esp_err_t err = gba_session_save_state(slot);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (err == ESP_OK) {
        return httpd_resp_send(req, "{\"ok\":true}", 11);
    } else {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"save failed\"}", 33);
    }
}

/* ---- POST /api/states/load → load state from slot ---- */
static esp_err_t states_load_handler(httpd_req_t *req)
{
    char body[64];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char *p = strstr(body, "\"slot\"");
    if (!p) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing slot");
        return ESP_FAIL;
    }
    p = strchr(p + 6, ':');
    if (!p) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    unsigned slot = (unsigned)atoi(p + 1);
    if (slot >= STATE_MAX_SLOTS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
        return ESP_FAIL;
    }

    esp_err_t err = gba_session_load_state(slot);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (err == ESP_OK) {
        return httpd_resp_send(req, "{\"ok\":true}", 11);
    } else {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"load failed\"}", 34);
    }
}

/* ---- GET /api/states/download?slot=N → download raw state file ---- */
static esp_err_t states_download_handler(httpd_req_t *req)
{
    /* Parse slot from query string */
    char qbuf[32];
    unsigned slot = 0;

    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(qbuf, "slot", val, sizeof(val)) == ESP_OK) {
            slot = (unsigned)atoi(val);
        }
    }
    if (slot >= STATE_MAX_SLOTS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
        return ESP_FAIL;
    }

    const char *rom_path = gba_session_current_rom_path();
    if (!rom_path) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no ROM loaded");
        return ESP_FAIL;
    }

    /* Get file path and open directly — no large allocation needed */
    char path[256];
    if (storage_get_state_path(rom_path, slot, path, sizeof(path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "state not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char disp[128];
    snprintf(disp, sizeof(disp), "attachment; filename=\"slot%u.state\"", slot);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    /* Stream file in 4 KB chunks — no heap allocation */
    char chunk[4096];
    esp_err_t send_err = ESP_OK;
    size_t n;
    while (send_err == ESP_OK && (n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        send_err = httpd_resp_send_chunk(req, chunk, n);
    }
    fclose(f);

    if (send_err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);  /* finish chunked response */
    }

    return send_err;
}

/* ---- GET /api/frame → GBA screen capture (raw RGB565) ---- */

#define FRAME_RAW_SIZE  (GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * 2)  /* 76800 bytes */

static esp_err_t frame_get_handler(httpd_req_t *req)
{
    if (!s_frame_mutex) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not ready");
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        return ESP_FAIL;
    }

    /* Allocate PSRAM copy buffer (64-byte aligned for cache ops). */
    uint8_t *fb_copy = heap_caps_aligned_alloc(64, FRAME_RAW_SIZE,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb_copy) {
        xSemaphoreGive(s_frame_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    /* Block until the AV task copies a completed frame into fb_copy. */
    esp_err_t snap = av_pipeline_snapshot_frame(fb_copy, FRAME_RAW_SIZE);
    if (snap != ESP_OK) {
        free(fb_copy);
        xSemaphoreGive(s_frame_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no frame");
        return ESP_FAIL;
    }

    /* Send raw RGB565 with metadata headers. */
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Format", "rgb565");
    httpd_resp_set_hdr(req, "X-Frame-Width", "240");
    httpd_resp_set_hdr(req, "X-Frame-Height", "160");
    esp_err_t ret = httpd_resp_send(req, (const char *)fb_copy, FRAME_RAW_SIZE);

    free(fb_copy);
    xSemaphoreGive(s_frame_mutex);
    return ret;
}

/* ---- POST /api/states/delete → delete a state file ---- */
static esp_err_t states_delete_handler(httpd_req_t *req)
{
    char body[64];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char *p = strstr(body, "\"slot\"");
    if (!p) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing slot");
        return ESP_FAIL;
    }
    p = strchr(p + 6, ':');
    if (!p) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    unsigned slot = (unsigned)atoi(p + 1);
    if (slot >= STATE_MAX_SLOTS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
        return ESP_FAIL;
    }

    const char *rom_path = gba_session_current_rom_path();
    if (!rom_path) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no ROM loaded");
        return ESP_FAIL;
    }

    esp_err_t err = storage_delete_state(rom_path, slot);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (err == ESP_OK) {
        return httpd_resp_send(req, "{\"ok\":true}", 11);
    } else {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"delete failed\"}", 35);
    }
}

/* ---- public API ---- */

uint16_t web_server_input_read(void)
{
    s_last_emu_read_us = esp_timer_get_time();
    uint16_t keys = s_web_keys_held | s_web_keys_pressed;
    s_web_keys_pressed = 0;
    return keys;
}

/* ---- H.264 + audio streaming ---- */

static void stream_free_buffers(void)
{
    heap_caps_free(s_stream.rgb_buf);     s_stream.rgb_buf    = NULL;
    heap_caps_free(s_stream.yuv_buf);     s_stream.yuv_buf    = NULL;
    heap_caps_free(s_stream.h264_buf);    s_stream.h264_buf   = NULL;
    heap_caps_free(s_stream.vid_pkt_buf); s_stream.vid_pkt_buf = NULL;
    heap_caps_free(s_stream.aud_pkt_buf); s_stream.aud_pkt_buf = NULL;
    heap_caps_free(s_stream.audio_buf);   s_stream.audio_buf  = NULL;
}

static bool stream_alloc_buffers(void)
{
    /* All buffers in PSRAM.  H264 buffers must be 16-byte aligned per esp_h264_alloc.h.
     * Use 64-byte alignment for cache-friendliness. */
    const uint32_t align = 64;
    const uint32_t rgb_size  = GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * 2;     /* 76800 */
    const uint32_t yuv_size  = STREAM_YUV_SIZE;                               /* 57600 */
    const uint32_t h264_size = STREAM_YUV_SIZE;                               /* worst-case NALs */
    const uint32_t vid_pkt_size = h264_size + 16;                             /* header + NAL */
    const uint32_t aud_pkt_size = STREAM_AUDIO_SAMPLES_MAX * 2 * sizeof(int16_t) + 16;
    const uint32_t aud_size  = STREAM_AUDIO_SAMPLES_MAX * 2 * sizeof(int16_t);

    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    s_stream.rgb_buf     = heap_caps_aligned_alloc(align, rgb_size,     caps);
    s_stream.yuv_buf     = heap_caps_aligned_alloc(align, yuv_size,     caps);
    s_stream.h264_buf    = heap_caps_aligned_alloc(align, h264_size,    caps);
    s_stream.vid_pkt_buf = heap_caps_aligned_alloc(align, vid_pkt_size, caps);
    s_stream.aud_pkt_buf = heap_caps_aligned_alloc(align, aud_pkt_size, caps);
    s_stream.audio_buf   = heap_caps_aligned_alloc(align, aud_size,     caps);

    s_stream.yuv_actual  = yuv_size;
    s_stream.h264_actual = h264_size;

    if (!s_stream.rgb_buf || !s_stream.yuv_buf || !s_stream.h264_buf ||
        !s_stream.vid_pkt_buf || !s_stream.aud_pkt_buf || !s_stream.audio_buf) {
        ESP_LOGE(TAG, "Stream buffer alloc failed");
        stream_free_buffers();
        return false;
    }
    ESP_LOGI(TAG, "Stream buffers allocated: %u bytes PSRAM",
             rgb_size + yuv_size + h264_size + vid_pkt_size + aud_pkt_size + aud_size);
    return true;
}

/* Lazily create PPA client + H264 encoder (persistent, never freed).
 * These leak a few KB on destroy, so we keep them alive forever. */
static esp_err_t stream_ensure_encoder(void)
{
    if (!s_stream.ppa_client) {
        ppa_client_config_t ppa_cfg = {
            .oper_type = PPA_OPERATION_SRM,
        };
        esp_err_t err = ppa_register_client(&ppa_cfg, &s_stream.ppa_client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "PPA client register failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (!s_stream.enc) {
        esp_h264_enc_cfg_hw_t enc_cfg = {
            .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
            .gop      = STREAM_GOP,
            .fps      = STREAM_FPS,
            .res      = { .width = GBA_SCREEN_WIDTH, .height = GBA_SCREEN_HEIGHT },
            .rc       = { .bitrate = STREAM_BITRATE, .qp_min = STREAM_QP_MIN, .qp_max = STREAM_QP_MAX },
        };
        esp_h264_err_t h264_ret = esp_h264_enc_hw_new(&enc_cfg, &s_stream.enc);
        if (h264_ret != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "H264 encoder create failed: %d", h264_ret);
            return ESP_FAIL;
        }

        h264_ret = esp_h264_enc_open(s_stream.enc);
        if (h264_ret != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "H264 encoder open failed: %d", h264_ret);
            esp_h264_enc_del(s_stream.enc);
            s_stream.enc = NULL;
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "H264 HW encoder ready (%ux%u, %ubps, GOP=%u)",
                 GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT, STREAM_BITRATE, STREAM_GOP);
    }

    return ESP_OK;
}

/* WS async send completion callback — signal stream to stop on error. */
static void stream_ws_send_cb(esp_err_t err, int sock, void *arg)
{
    if (err != ESP_OK && s_stream.ws_fd == sock) {
        ESP_LOGW(TAG, "Stream WS send failed fd=%d: %s, stopping", sock, esp_err_to_name(err));
        s_stream.ws_fd = -1;
        s_stream.active = false;
    }
}

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

static void stream_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Stream task started");

    const uint32_t rgb_frame_size = GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * 2;
    s_stream.frame_count = 0;

    while (s_stream.active && s_stream.ws_fd >= 0) {

        /* ----- 1. Snapshot RGB565 frame from AV pipeline -----
         * Blocks until the next GBA frame is ready (~16.7ms at 59.7 FPS).
         * Paced by GBA's native frame rate. */
        esp_err_t snap = av_pipeline_snapshot_frame(s_stream.rgb_buf, rgb_frame_size);
        if (snap != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(8));
            continue;
        }

        /* ----- 2. Interleave audio and video on alternating frames -----
         * Even frames: encode + send video (30fps H.264)
         * Odd frames:  drain + send audio (30fps, covers ~2 GBA frames)
         * This avoids back-to-back large WS sends that overflow WiFi TX. */

        if ((s_stream.frame_count & 1) != 0 && s_stream.ws_fd >= 0) {
            /* ODD frame → send accumulated audio */
            if (av_pipeline_audio_enabled()) {
                const uint32_t max_audio_bytes = STREAM_AUDIO_SAMPLES_MAX * 2 * sizeof(int16_t);
                size_t bytes = av_pipeline_stream_audio_read(s_stream.audio_buf, max_audio_bytes);
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

        if ((s_stream.frame_count & 1) == 0 && s_stream.ws_fd >= 0) {
            /* EVEN frame → encode and send video */
            ppa_srm_oper_config_t srm_cfg = {
                .in = {
                    .buffer     = s_stream.rgb_buf,
                    .pic_w      = GBA_SCREEN_WIDTH,
                    .pic_h      = GBA_SCREEN_HEIGHT,
                    .block_w    = GBA_SCREEN_WIDTH,
                    .block_h    = GBA_SCREEN_HEIGHT,
                    .block_offset_x = 0,
                    .block_offset_y = 0,
                    .srm_cm     = PPA_SRM_COLOR_MODE_RGB565,
                },
                .out = {
                    .buffer      = s_stream.yuv_buf,
                    .buffer_size = s_stream.yuv_actual,
                    .pic_w       = GBA_SCREEN_WIDTH,
                    .pic_h       = GBA_SCREEN_HEIGHT,
                    .block_offset_x = 0,
                    .block_offset_y = 0,
                    .srm_cm      = PPA_SRM_COLOR_MODE_YUV420,
                },
                .rotation_angle  = PPA_SRM_ROTATION_ANGLE_0,
                .scale_x         = 1.0f,
                .scale_y         = 1.0f,
                .rgb_swap        = false,
                .byte_swap       = false,
                .mode            = PPA_TRANS_MODE_BLOCKING,
            };
            esp_err_t ppa_ret = ppa_do_scale_rotate_mirror(s_stream.ppa_client, &srm_cfg);
            if (ppa_ret != ESP_OK) {
                s_stream.frame_count++;
                continue;
            }

            esp_h264_enc_in_frame_t in_frame = {
                .raw_data = { .buffer = s_stream.yuv_buf, .len = s_stream.yuv_actual },
                .pts = s_stream.frame_count,
            };
            esp_h264_enc_out_frame_t out_frame = {
                .raw_data = { .buffer = s_stream.h264_buf, .len = s_stream.h264_actual },
            };
            esp_h264_err_t h264_ret = esp_h264_enc_process(s_stream.enc, &in_frame, &out_frame);
            if (h264_ret != ESP_H264_ERR_OK) {
                s_stream.frame_count++;
                continue;
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

    ESP_LOGI(TAG, "Stream task ending, freeing buffers");
    av_pipeline_stream_audio_stop();
    stream_free_buffers();
    s_stream.active = false;
    /* Do NOT use vTaskDelete(NULL) — task was created with
     * xTaskCreatePinnedToCoreWithCaps so the stack would leak.
     * Suspend self and let the handler reap us. */
    s_stream.task_done = true;
    vTaskSuspend(NULL);
}

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

/* WebSocket handler for /ws/stream */
static esp_err_t ws_stream_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* New stream client connected — just record the fd.
         * All heavy work (alloc, encoder, task) deferred to 0x20
         * to avoid blocking the httpd socket-accept path. */
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "Stream WS client connected, fd=%d", fd);
        s_stream.ws_fd = fd;
        return ESP_OK;
    }

    /* Handle incoming WS messages (e.g. client disconnect detection) */
    httpd_ws_frame_t ws_pkt = {0};
    uint8_t buf[8];
    ws_pkt.payload = buf;
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf));
    if (ret != ESP_OK) {
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "Stream WS recv failed on fd=%d", fd);
        /* Only tear down if this fd is still the active stream fd.
         * A newer connection may have already replaced us. */
        if (s_stream.ws_fd == fd) {
            bool was_active = s_stream.active;
            s_stream.ws_fd = -1;
            s_stream.active = false;
            if (was_active) {
                vTaskDelay(pdMS_TO_TICKS(50));
                stream_reap_task();
            }
            stream_free_buffers();
        }
        return ESP_FAIL;
    }

    /* 0x10 = stop streaming command from client */
    if (ws_pkt.len >= 1 && buf[0] == 0x10) {
        int fd = httpd_req_to_sockfd(req);
        if (s_stream.ws_fd == fd) {
            ESP_LOGI(TAG, "Stream stop requested by client");
            s_stream.ws_fd = -1;
            s_stream.active = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            stream_reap_task();
            stream_free_buffers();
        }
    }

    /* 0x20 = reset/start command from client (decoder ready).
     * All resource allocation happens here, NOT in the GET handler,
     * to avoid blocking the httpd socket-accept path. */
    if (ws_pkt.len >= 1 && buf[0] == 0x20) {
        int fd = httpd_req_to_sockfd(req);
        if (s_stream.ws_fd != fd) {
            ESP_LOGW(TAG, "Ignoring reset from stale fd=%d (current=%d)", fd, s_stream.ws_fd);
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Stream reset requested by client (fd=%d)", fd);

        /* Tear down any previous session */
        if (s_stream.active) {
            s_stream.active = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            stream_reap_task();
        } else {
            stream_reap_task();
        }
        stream_free_buffers();

        /* Allocate buffers + ensure encoder */
        if (!stream_alloc_buffers()) {
            return ESP_OK;
        }
        if (stream_ensure_encoder() != ESP_OK) {
            stream_free_buffers();
            return ESP_OK;
        }

        /* Reset encoder → first frame will be IDR */
        esp_h264_enc_close(s_stream.enc);
        if (esp_h264_enc_open(s_stream.enc) != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "H264 encoder reopen failed");
            stream_free_buffers();
            return ESP_OK;
        }
        av_pipeline_stream_audio_start();

        s_stream.active = true;
        s_stream.frame_count = 0;

        /* Start streaming task on service core */
        BaseType_t core = (CONFIG_GPSP_EMULATION_CORE == 0) ? 1 : 0;
        BaseType_t xret = xTaskCreatePinnedToCoreWithCaps(
            stream_task, "av_stream",
            6144, NULL,
            tskIDLE_PRIORITY + 3,
            &s_stream.task,
            core,
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

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 20;
    config.stack_size = 8192;

    /* Init frame capture mutex once */
    if (!s_frame_mutex) {
        s_frame_mutex = xSemaphoreCreateMutex();
    }

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Root page */
    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_register_uri_handler(s_server, &root_uri);

    /* WebSocket endpoint */
    const httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    /* Input controller page */
    const httpd_uri_t input_uri = {
        .uri = "/input",
        .method = HTTP_GET,
        .handler = input_get_handler,
    };
    httpd_register_uri_handler(s_server, &input_uri);

    /* Stats JSON endpoint */
    const httpd_uri_t stats_uri = {
        .uri = "/api/stats",
        .method = HTTP_GET,
        .handler = stats_get_handler,
    };
    httpd_register_uri_handler(s_server, &stats_uri);

    /* Settings GET */
    const httpd_uri_t settings_get_uri = {
        .uri = "/api/settings",
        .method = HTTP_GET,
        .handler = settings_get_handler,
    };
    httpd_register_uri_handler(s_server, &settings_get_uri);

    /* Settings POST */
    const httpd_uri_t settings_post_uri = {
        .uri = "/api/settings",
        .method = HTTP_POST,
        .handler = settings_post_handler,
    };
    httpd_register_uri_handler(s_server, &settings_post_uri);

    /* ROM list */
    const httpd_uri_t roms_get_uri = {
        .uri = "/api/roms",
        .method = HTTP_GET,
        .handler = roms_get_handler,
    };
    httpd_register_uri_handler(s_server, &roms_get_uri);

    /* ROM load */
    const httpd_uri_t roms_load_uri = {
        .uri = "/api/roms/load",
        .method = HTTP_POST,
        .handler = roms_load_handler,
    };
    httpd_register_uri_handler(s_server, &roms_load_uri);

    /* State list */
    const httpd_uri_t states_get_uri = {
        .uri = "/api/states",
        .method = HTTP_GET,
        .handler = states_get_handler,
    };
    httpd_register_uri_handler(s_server, &states_get_uri);

    /* State save */
    const httpd_uri_t states_save_uri = {
        .uri = "/api/states/save",
        .method = HTTP_POST,
        .handler = states_save_handler,
    };
    httpd_register_uri_handler(s_server, &states_save_uri);

    /* State load */
    const httpd_uri_t states_load_uri = {
        .uri = "/api/states/load",
        .method = HTTP_POST,
        .handler = states_load_handler,
    };
    httpd_register_uri_handler(s_server, &states_load_uri);

    /* State download */
    const httpd_uri_t states_download_uri = {
        .uri = "/api/states/download",
        .method = HTTP_GET,
        .handler = states_download_handler,
    };
    httpd_register_uri_handler(s_server, &states_download_uri);

    /* State delete */
    const httpd_uri_t states_delete_uri = {
        .uri = "/api/states/delete",
        .method = HTTP_POST,
        .handler = states_delete_handler,
    };
    httpd_register_uri_handler(s_server, &states_delete_uri);

    /* Frame capture */
    const httpd_uri_t frame_get_uri = {
        .uri = "/api/frame",
        .method = HTTP_GET,
        .handler = frame_get_handler,
    };
    httpd_register_uri_handler(s_server, &frame_get_uri);

    /* A/V stream WebSocket */
    const httpd_uri_t ws_stream_uri = {
        .uri = "/ws/stream",
        .method = HTTP_GET,
        .handler = ws_stream_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws_stream_uri);

    /* Init stream state */
    s_stream.ws_fd = -1;
    s_stream.active = false;

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}

/* ---- async startup task ---- */
static void web_server_start_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Waiting for network before starting web server...");
    while (!c6_remote_network_ready()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "Network ready, starting web server");
    esp_err_t err = web_server_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Web server failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

void web_server_start_async(int core_id)
{
    xTaskCreatePinnedToCoreWithCaps(web_server_start_task, "web_srv_init", 4096, NULL,
                                    tskIDLE_PRIORITY + 1, NULL, core_id,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
