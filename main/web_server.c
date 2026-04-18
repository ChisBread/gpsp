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
#include "av_stream.h"
#include "runtime_config.h"
#include "gba_session.h"
#include "netpacket_tunnel_host.h"
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
#include <lwip/sockets.h>
#include <lwip/netdb.h>

#include "av_pipeline.h"
#include "common.h"
#include "cpu.h"
#include "gba_memory.h"
#include "main.h"
#include "savestate.h"
#include "serial.h"

static const char *TAG = "web_server";

/* ---- state ---- */
static httpd_handle_t s_server;
static volatile uint16_t s_web_keys_held;     /* last WS state */
static volatile uint16_t s_web_keys_pressed;  /* OR-accumulated presses since last emu read */
static volatile int64_t  s_last_recv_us;      /* timestamp of last WS recv */
static volatile int64_t  s_last_emu_read_us;  /* timestamp of last emu read */

/* ---- frame capture mutex (serialises /api/frame requests) ---- */
static SemaphoreHandle_t s_frame_mutex;

/* ---- embedded HTML ---- */
extern const char web_home_html_start[] asm("_binary_web_home_html_start");
extern const char web_home_html_end[]   asm("_binary_web_home_html_end");
extern const char web_server_html_start[] asm("_binary_web_server_html_start");
extern const char web_server_html_end[]   asm("_binary_web_server_html_end");

/* ---- HTTP GET /favicon.ico → 204 No Content ---- */
static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

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

static const char *serial_setting_str(int s)
{
    switch (s) {
    case SERIAL_MODE_DISABLED:    return "disabled";
    case SERIAL_MODE_RFU:         return "rfu";
    case SERIAL_MODE_SERIAL_POKE: return "mul_poke";
    case SERIAL_MODE_SERIAL_AW1:  return "mul_aw1";
    case SERIAL_MODE_SERIAL_AW2:  return "mul_aw2";
    default:                      return "auto";
    }
}

static const char *rtc_mode_str(int m)
{
    switch (m) {
    case FEAT_DISABLE: return "disabled";
    case FEAT_ENABLE:  return "enabled";
    default:           return "auto";
    }
}

/* ---- GET /api/settings → current settings JSON ---- */
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    char buf[1536];
    int len = snprintf(buf, sizeof(buf),
        "{\"dynarec_enable\":%d,\"sprite_limit\":%d,\"boot_mode\":\"%s\""
        ",\"serial_mode\":\"%s\",\"rtc_mode\":\"%s\""
        ",\"frameskip_type\":%u,\"frameskip_interval\":%u,\"frameskip_threshold\":%u"
        ",\"netplay_role\":%d,\"netplay_use_tunnel\":%d,\"netplay_use_lobby\":%d"
        ",\"netplay_host\":\"%s\",\"netplay_port\":%u,\"netplay_nick\":\"%s\""
        ",\"netplay_tunnel_id\":\"%s\""
        ",\"netplay_host_password\":\"%s\",\"netplay_client_password\":\"%s\""
        ",\"netplay_lobby_host\":\"%s\",\"netplay_lobby_port\":%u"
        ",\"netplay_lobby_relay\":\"%s\",\"netplay_lobby_country\":\"%s\""
        ",\"netplay_tunnel_session_id\":\"%s\",\"netplay_tunnel_status\":\"%s\""
        ",\"netplay_lobby_room_id\":\"%s\""
        ",\"netplay_enable\":%d,\"netplay_mode\":%d}",
        dynarec_enable ? 1 : 0,
        sprite_limit ? 1 : 0,
        selected_boot_mode == boot_bios ? "bios" : "game",
        serial_setting_str(gpsp_serial_setting),
        rtc_mode_str(gpsp_rtc_mode),
        (unsigned)gpsp_frameskip_type,
        (unsigned)gpsp_frameskip_interval,
        (unsigned)gpsp_frameskip_threshold,
        gpsp_netplay_role,
        gpsp_netplay_use_tunnel ? 1 : 0,
        gpsp_netplay_use_lobby ? 1 : 0,
        gpsp_netplay_ra_host,
        gpsp_netplay_ra_port,
        gpsp_netplay_ra_nick,
        gpsp_netplay_ra_tunnel_id,
        gpsp_netplay_host_password,
        gpsp_netplay_client_password,
        gpsp_netplay_lobby_host,
        gpsp_netplay_lobby_port,
        gpsp_netplay_lobby_relay,
        gpsp_netplay_lobby_country,
        netpacket_tunnel_host_session_id(),
        netpacket_tunnel_host_status(),
        netpacket_tunnel_host_room_id(),
        gpsp_netplay_ra_enabled ? 1 : 0,
        gpsp_netplay_ra_mode);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, len);
}

/* ---- POST /api/settings → apply + save ---- */
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char body[640];
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

    p = strstr(body, "\"serial_mode\"");
    if (p) {
        if (strstr(p, "\"rfu\""))
            gpsp_serial_setting = SERIAL_MODE_RFU;
        else if (strstr(p, "\"mul_poke\""))
            gpsp_serial_setting = SERIAL_MODE_SERIAL_POKE;
        else if (strstr(p, "\"mul_aw1\""))
            gpsp_serial_setting = SERIAL_MODE_SERIAL_AW1;
        else if (strstr(p, "\"mul_aw2\""))
            gpsp_serial_setting = SERIAL_MODE_SERIAL_AW2;
        else if (strstr(p, "\"disabled\""))
            gpsp_serial_setting = SERIAL_MODE_DISABLED;
        else
            gpsp_serial_setting = SERIAL_MODE_AUTO;
    }

    p = strstr(body, "\"rtc_mode\"");
    if (p) {
        if (strstr(p, "\"enabled\""))
            gpsp_rtc_mode = FEAT_ENABLE;
        else if (strstr(p, "\"disabled\""))
            gpsp_rtc_mode = FEAT_DISABLE;
        else
            gpsp_rtc_mode = FEAT_AUTODETECT;
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

    p = strstr(body, "\"netplay_role\"");
    if (p) {
        p = strchr(p + 14, ':');
        if (p) {
            int v = atoi(p + 1);
            if (v >= 0 && v <= 2)
                gpsp_netplay_role = v;
        }
    }

    p = strstr(body, "\"netplay_use_tunnel\"");
    if (p) {
        p = strchr(p + 20, ':');
        if (p) gpsp_netplay_use_tunnel = atoi(p + 1) ? true : false;
    }

    p = strstr(body, "\"netplay_use_lobby\"");
    if (p) {
        p = strchr(p + 19, ':');
        if (p) gpsp_netplay_use_lobby = atoi(p + 1) ? true : false;
    }

    /* Legacy: netplay_mode still accepted for backward compat */
    p = strstr(body, "\"netplay_mode\"");
    if (p) {
        p = strchr(p + 14, ':');
        if (p) {
            int v = atoi(p + 1);
            if (v >= 0 && v <= 4) {
                gpsp_netplay_ra_mode = v;
                gpsp_netplay_ra_enabled = (v != 0);
            }
        }
    }

    p = strstr(body, "\"netplay_host\"");
    if (p) {
        p = strchr(p + 14, ':');
        if (p) {
            /* Extract string value between quotes */
            char *q = strchr(p, '"');
            if (q) {
                q++;
                char *e = strchr(q, '"');
                if (e) {
                    size_t len = (size_t)(e - q);
                    if (len >= sizeof(gpsp_netplay_ra_host))
                        len = sizeof(gpsp_netplay_ra_host) - 1;
                    memcpy(gpsp_netplay_ra_host, q, len);
                    gpsp_netplay_ra_host[len] = '\0';
                }
            }
        }
    }

    p = strstr(body, "\"netplay_port\"");
    if (p) {
        p = strchr(p + 14, ':');
        if (p) {
            int v = atoi(p + 1);
            if (v >= 1 && v <= 65535)
                gpsp_netplay_ra_port = (uint16_t)v;
        }
    }

    p = strstr(body, "\"netplay_nick\"");
    if (p) {
        p = strchr(p + 14, ':');
        if (p) {
            char *q = strchr(p, '"');
            if (q) {
                q++;
                char *e = strchr(q, '"');
                if (e) {
                    size_t len = (size_t)(e - q);
                    if (len >= sizeof(gpsp_netplay_ra_nick))
                        len = sizeof(gpsp_netplay_ra_nick) - 1;
                    memcpy(gpsp_netplay_ra_nick, q, len);
                    gpsp_netplay_ra_nick[len] = '\0';
                }
            }
        }
    }

    p = strstr(body, "\"netplay_tunnel_id\"");
    if (p) {
        p = strchr(p + 19, ':');
        if (p) {
            char *q = strchr(p, '"');
            if (q) {
                q++;
                char *e = strchr(q, '"');
                if (e) {
                    size_t len = (size_t)(e - q);
                    if (len >= sizeof(gpsp_netplay_ra_tunnel_id))
                        len = sizeof(gpsp_netplay_ra_tunnel_id) - 1;
                    memcpy(gpsp_netplay_ra_tunnel_id, q, len);
                    gpsp_netplay_ra_tunnel_id[len] = '\0';
                }
            }
        }
    }

    p = strstr(body, "\"netplay_lobby_host\"");
    if (p) {
        p = strchr(p + 20, ':');
        if (p) {
            char *q = strchr(p, '"');
            if (q) {
                q++;
                char *e = strchr(q, '"');
                if (e) {
                    size_t len = (size_t)(e - q);
                    if (len >= sizeof(gpsp_netplay_lobby_host))
                        len = sizeof(gpsp_netplay_lobby_host) - 1;
                    memcpy(gpsp_netplay_lobby_host, q, len);
                    gpsp_netplay_lobby_host[len] = '\0';
                }
            }
        }
    }

    p = strstr(body, "\"netplay_lobby_port\"");
    if (p) {
        p = strchr(p + 20, ':');
        if (p) {
            int v = atoi(p + 1);
            if (v >= 1 && v <= 65535)
                gpsp_netplay_lobby_port = (uint16_t)v;
        }
    }

    p = strstr(body, "\"netplay_lobby_relay\"");
    if (p) {
        p = strchr(p + 21, ':');
        if (p) {
            char *q = strchr(p, '"');
            if (q) {
                q++;
                char *e = strchr(q, '"');
                if (e) {
                    size_t len = (size_t)(e - q);
                    if (len >= sizeof(gpsp_netplay_lobby_relay))
                        len = sizeof(gpsp_netplay_lobby_relay) - 1;
                    memcpy(gpsp_netplay_lobby_relay, q, len);
                    gpsp_netplay_lobby_relay[len] = '\0';
                }
            }
        }
    }

    /* Extract string helper macro for password/country fields */
#define EXTRACT_STR(json_key, dst, dst_size) do {                       \
    p = strstr(body, "\"" json_key "\"");                               \
    if (p) {                                                            \
        p = strchr(p + sizeof(json_key) + 1, ':');                      \
        if (p) {                                                        \
            char *_q = strchr(p, '"');                                   \
            if (_q) {                                                    \
                _q++;                                                    \
                char *_e = strchr(_q, '"');                              \
                if (_e) {                                                \
                    size_t _len = (size_t)(_e - _q);                    \
                    if (_len >= (dst_size)) _len = (dst_size) - 1;      \
                    memcpy((dst), _q, _len);                            \
                    (dst)[_len] = '\0';                                  \
                }                                                        \
            }                                                            \
        }                                                                \
    }                                                                    \
} while (0)

    EXTRACT_STR("netplay_host_password",   gpsp_netplay_host_password,   sizeof(gpsp_netplay_host_password));
    EXTRACT_STR("netplay_client_password", gpsp_netplay_client_password, sizeof(gpsp_netplay_client_password));
    EXTRACT_STR("netplay_lobby_country",   gpsp_netplay_lobby_country,   sizeof(gpsp_netplay_lobby_country));

#undef EXTRACT_STR

    gpsp_runtime_config_save();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

/* ---- POST /api/netplay/apply → derive mode from role+tunnel and activate ---- */
static esp_err_t netplay_apply_handler(httpd_req_t *req)
{
    gpsp_netplay_update_mode();
    gpsp_runtime_config_save();

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"netplay_enable\":%d,\"netplay_mode\":%d"
        ",\"netplay_tunnel_session_id\":\"%s\",\"netplay_tunnel_status\":\"%s\"}",
        gpsp_netplay_ra_enabled ? 1 : 0,
        gpsp_netplay_ra_mode,
        netpacket_tunnel_host_session_id(),
        netpacket_tunnel_host_status());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, len);
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

/* ---- POST /api/upload → chunked file upload to SD card ----
 *
 * The frontend slices a file into small pieces (~64 KB) and sends
 * each as a separate POST with query parameters:
 *
 *   POST /api/upload?name=<filename>&offset=<N>&total=<T>
 *   Body: raw chunk bytes (application/octet-stream)
 *
 * offset=0 creates/truncates the file.  Subsequent requests append.
 * This avoids holding large data in RAM and tolerates slow SD writes
 * because each HTTP transaction is small and self-contained.
 */

#define UPLOAD_BUF_SIZE  4096  /* stack recv buffer per httpd_req_recv */

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char qbuf[384], val[256];
    char filename[128] = {0};
    size_t offset = 0, total = 0;

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
        return ESP_FAIL;
    }
    if (httpd_query_key_value(qbuf, "name", val, sizeof(val)) != ESP_OK || val[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name");
        return ESP_FAIL;
    }

    /* Sanitise: basename only, no ".." */
    {
        char *p = strrchr(val, '/');
        if (!p) p = strrchr(val, '\\');
        const char *base = p ? p + 1 : val;
        if (base[0] == '\0' || strstr(base, "..")) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
            return ESP_FAIL;
        }
        strlcpy(filename, base, sizeof(filename));
    }

    if (httpd_query_key_value(qbuf, "offset", val, sizeof(val)) == ESP_OK)
        offset = (size_t)strtoul(val, NULL, 10);
    if (httpd_query_key_value(qbuf, "total", val, sizeof(val)) == ESP_OK)
        total = (size_t)strtoul(val, NULL, 10);

    char filepath[300];
    snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_MOUNT_POINT, filename);

    /* Open: truncate on first chunk, append on subsequent */
    FILE *fp = fopen(filepath, offset == 0 ? "wb" : "ab");
    if (!fp) {
        ESP_LOGE(TAG, "Upload: cannot open %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_FAIL;
    }

    if (offset == 0) {
        ESP_LOGI(TAG, "Upload start: %s (total %zu bytes)", filepath, total);
    }

    /* Receive body and write directly to SD */
    char buf[UPLOAD_BUF_SIZE];
    size_t remaining = req->content_len;
    size_t written = 0;

    while (remaining > 0) {
        size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
        int got = httpd_req_recv(req, buf, to_read);
        if (got <= 0) {
            ESP_LOGE(TAG, "Upload recv error at offset %zu+%zu: %d",
                     offset, written, got);
            fclose(fp);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        size_t w = fwrite(buf, 1, got, fp);
        if (w != (size_t)got) {
            ESP_LOGE(TAG, "Upload SD write error at offset %zu+%zu", offset, written);
            fclose(fp);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
            return ESP_FAIL;
        }
        written += got;
        remaining -= got;
    }

    fclose(fp);

    bool is_last = (total > 0) && (offset + written >= total);
    if (is_last) {
        ESP_LOGI(TAG, "Upload done: %s (%zu bytes)", filepath, offset + written);
    }

    httpd_resp_set_type(req, "application/json");
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"written\":%zu,\"offset\":%zu}", written, offset);
    return httpd_resp_send(req, resp, strlen(resp));
}

/* ---- public API ---- */

uint16_t web_server_input_read(void)
{
    s_last_emu_read_us = esp_timer_get_time();
    uint16_t keys = s_web_keys_held | s_web_keys_pressed;
    s_web_keys_pressed = 0;
    return keys;
}

/* ---- GET /api/lobby/room?id=<roomID>
 *      Proxies GET /<roomID> to the lobby server and extracts mitm_session
 *      (base64-encoded 12-byte session ID).
 *      Returns: {"session_id":"<base64>"} or {"error":"..."} */
static esp_err_t lobby_room_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (!gpsp_netplay_lobby_host[0] || !gpsp_netplay_lobby_port) {
        return httpd_resp_send(req, "{\"error\":\"lobby not configured\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* Extract ?id= from query string */
    char query[32];
    char room_str[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "id", room_str, sizeof(room_str));
    }
    if (!room_str[0]) {
        return httpd_resp_send(req, "{\"error\":\"missing id\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* Resolve and connect to lobby */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    struct hostent *he = gethostbyname(gpsp_netplay_lobby_host);
    if (!he) {
        return httpd_resp_send(req, "{\"error\":\"lobby dns failed\"}", HTTPD_RESP_USE_STRLEN);
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(gpsp_netplay_lobby_port);
    addr.sin_addr = *(struct in_addr *)he->h_addr;

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return httpd_resp_send(req, "{\"error\":\"socket failed\"}", HTTPD_RESP_USE_STRLEN);
    }

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return httpd_resp_send(req, "{\"error\":\"lobby connect failed\"}", HTTPD_RESP_USE_STRLEN);
    }

    char req_buf[128];
    int req_len = snprintf(req_buf, sizeof(req_buf),
        "GET /%s HTTP/1.0\r\nHost: %s:%u\r\nConnection: close\r\n\r\n",
        room_str, gpsp_netplay_lobby_host, (unsigned)gpsp_netplay_lobby_port);
    send(fd, req_buf, req_len, 0);

    /* Read full response */
    char resp[2048];
    size_t got = 0;
    ssize_t n;
    while (got + 1 < sizeof(resp) && (n = recv(fd, resp + got, sizeof(resp) - got - 1, 0)) > 0)
        got += (size_t)n;
    resp[got] = '\0';
    close(fd);

    if (!strstr(resp, " 200 ")) {
        return httpd_resp_send(req, "{\"error\":\"lobby returned non-200\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* Find "mitm_session":"<base64>" in the JSON body */
    char *tag = strstr(resp, "\"mitm_session\"");
    if (!tag) {
        return httpd_resp_send(req, "{\"error\":\"no mitm_session field\"}", HTTPD_RESP_USE_STRLEN);
    }
    char *p = strchr(tag + 14, '"');
    if (!p) {
        return httpd_resp_send(req, "{\"error\":\"bad mitm_session format\"}", HTTPD_RESP_USE_STRLEN);
    }
    p++; /* skip opening quote */
    char *e = strchr(p, '"');
    if (!e || e - p < 1 || e - p > 64) {
        return httpd_resp_send(req, "{\"error\":\"bad mitm_session value\"}", HTTPD_RESP_USE_STRLEN);
    }
    *e = '\0';

    char out[128];
    snprintf(out, sizeof(out), "{\"session_id\":\"%s\"}", p);
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 20;
    config.max_open_sockets = 10;  /* save sockets for netplay + WS */
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

    av_stream_set_server(s_server);

    /* Root page */
    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_register_uri_handler(s_server, &root_uri);

    /* Favicon — suppress browser 404 noise */
    const httpd_uri_t favicon_uri = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_handler,
    };
    httpd_register_uri_handler(s_server, &favicon_uri);

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

    /* Netplay apply */
    const httpd_uri_t netplay_apply_uri = {
        .uri = "/api/netplay/apply",
        .method = HTTP_POST,
        .handler = netplay_apply_handler,
    };
    httpd_register_uri_handler(s_server, &netplay_apply_uri);

    /* Lobby room lookup proxy */
    const httpd_uri_t lobby_room_uri = {
        .uri = "/api/lobby/room",
        .method = HTTP_GET,
        .handler = lobby_room_get_handler,
    };
    httpd_register_uri_handler(s_server, &lobby_room_uri);

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

    /* File upload to SD card */
    const httpd_uri_t upload_uri = {
        .uri = "/api/upload",
        .method = HTTP_POST,
        .handler = upload_post_handler,
    };
    httpd_register_uri_handler(s_server, &upload_uri);

    /* A/V stream WebSocket */
    const httpd_uri_t ws_stream_uri = {
        .uri = "/ws/stream",
        .method = HTTP_GET,
        .handler = av_stream_ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws_stream_uri);

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
