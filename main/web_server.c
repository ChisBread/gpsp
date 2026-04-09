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
#include "esp32p4/input_driver.h"
#include "c6_remote.h"

#include <string.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "common.h"
#include "cpu.h"
#include "main.h"

static const char *TAG = "web_server";

/* ---- state ---- */
static httpd_handle_t s_server;
static volatile uint16_t s_web_keys;          /* atomic on 32-bit arch */
static volatile int64_t  s_last_recv_us;      /* timestamp of last WS recv */
static volatile int64_t  s_last_emu_read_us;  /* timestamp of last emu read */

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
        uint16_t keys = (uint16_t)(buf[1] | (buf[2] << 8));
        uint32_t client_ts = buf[3] | (buf[4] << 8) | (buf[5] << 16) | (buf[6] << 24);

        s_web_keys = keys & 0x3FF;
        s_last_recv_us = now_us;

        /* Compute emu latency: time since last emu read (how stale was previous input) */
        int64_t emu_lat_us = 0;
        int64_t last_read = s_last_emu_read_us;
        if (last_read > 0 && s_last_recv_us > last_read) {
            /* This represents "how long until emu picks up this change" - roughly next frame */
            emu_lat_us = 0; /* Will be filled on next read */
        }

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
    char buf[1024];
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
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"dynarec_enable\":%d,\"sprite_limit\":%d,\"boot_mode\":\"%s\"}",
        dynarec_enable ? 1 : 0,
        sprite_limit ? 1 : 0,
        selected_boot_mode == boot_bios ? "bios" : "game");

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

    gpsp_runtime_config_save();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

/* ---- public API ---- */

uint16_t web_server_input_read(void)
{
    s_last_emu_read_us = esp_timer_get_time();
    return s_web_keys;
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    config.stack_size = 8192;

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
    xTaskCreatePinnedToCore(web_server_start_task, "web_srv_init", 4096, NULL,
                           tskIDLE_PRIORITY + 1, NULL, core_id);
}
