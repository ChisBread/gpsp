/*
 * gpSP app support — RetroArch tunnel host control path for ESP32-P4
 *
 * Creates a tunnel session on a RetroArch-compatible relay and attaches
 * per-peer forwarding sockets into the existing host-side RA protocol logic.
 */

#include "netpacket_tunnel_host.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "c6_remote.h"
#include "gpsp_config.h"
#include "runtime_config.h"

/* Lobby registration disguise — change these to impersonate another platform */
#define LOBBY_CORE_NAME      "gpSP"
#define LOBBY_CORE_VERSION   "v1.1.0-6373ff3"
#define LOBBY_RA_VERSION     "1.22.2"
#define LOBBY_FRONTEND       "unix x64"
#define LOBBY_SUBSYSTEM      "N/A"
#define LOBBY_GAME_NAME      "ChisGBA"
#define LOBBY_GAME_CRC       "00000000"

extern bool netpacket_host_attach_client(int fd, const char *peer_desc);

#define TUNNEL_MAGIC_SESSION  "RATS"
#define TUNNEL_MAGIC_LINK     "RATL"
#define TUNNEL_MAGIC_PING     "RATP"
#define TUNNEL_UNIQUE_SIZE    12
#define TUNNEL_MSG_SIZE       16
#define TUNNEL_MAX_PENDING    3
#define LOBBY_POST_INTERVAL_US 20000000

typedef enum {
    TUNNEL_CTRL_DISCONNECTED = 0,
    TUNNEL_CTRL_CONNECTING,
    TUNNEL_CTRL_WAIT_SESSION,
    TUNNEL_CTRL_READY,
} tunnel_ctrl_state_t;

typedef enum {
    LINK_SLOT_EMPTY = 0,
    LINK_SLOT_CONNECTING,
} tunnel_link_state_t;

typedef struct {
    int fd;
    tunnel_link_state_t state;
    uint8_t peer_id[TUNNEL_UNIQUE_SIZE];
    char peer_hex[TUNNEL_UNIQUE_SIZE * 2 + 1];
} pending_link_t;

static const char *TAG = "gpsp_tunnel_host";

static int ctrl_fd = -1;
static tunnel_ctrl_state_t ctrl_state;
static int64_t ctrl_last_connect_attempt_us;
static char ctrl_session_id[TUNNEL_UNIQUE_SIZE * 2 + 1];
static char ctrl_status[32] = "idle";
static uint8_t ctrl_recv_buf[64];
static size_t ctrl_recv_len;
static uint8_t ctrl_shadow_buf[64];
static size_t ctrl_shadow_len;
static bool ctrl_shadow_closed;
static char lobby_room_id[24];
static pending_link_t pending_links[TUNNEL_MAX_PENDING];
static int64_t lobby_last_post_us;
static SemaphoreHandle_t ctrl_shadow_mutex;
static TaskHandle_t tunnel_io_task_handle;

static void tunnel_ctrl_io_task(void *param);

void netpacket_tunnel_host_notify_io_task(void)
{
    if (tunnel_io_task_handle)
        xTaskNotifyGive(tunnel_io_task_handle);
}

/* Forward declarations for helpers referenced before their definitions */
static bool set_nonblocking_nodelay(int fd);
static bool resolve_host_addr(const char *host, uint16_t port,
                              struct sockaddr_in *out_addr);
static bool socket_connect_done(int fd);
static bool socket_connect_wait(int fd, int timeout_ms);

static void percent_encode_append(char *dst, size_t dst_size,
                                  size_t *io_off, const char *src)
{
    size_t off = io_off ? *io_off : 0;
    const char *hex = "0123456789ABCDEF";

    if (!dst || dst_size == 0 || !io_off)
        return;

    if (!src)
        src = "";

    while (*src && off + 1 < dst_size) {
        unsigned char c = (unsigned char)*src++;
        bool safe = (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~';

        if (safe) {
            dst[off++] = (char)c;
            continue;
        }

        if (off + 3 >= dst_size)
            break;
        dst[off++] = '%';
        dst[off++] = hex[(c >> 4) & 0x0f];
        dst[off++] = hex[c & 0x0f];
    }

    dst[off] = '\0';
    *io_off = off;
}

static bool lobby_enabled(void)
{
    return gpsp_netplay_use_lobby &&
           gpsp_netplay_lobby_host[0] != '\0' &&
           gpsp_netplay_lobby_port != 0 &&
           gpsp_netplay_lobby_relay[0] != '\0';
}

static bool send_with_timeout(int fd, const void *buf, size_t len, int timeout_ms)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (esp_timer_get_time() > deadline)
                return false;
            fd_set wfds;
            struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            (void)select(fd + 1, NULL, &wfds, NULL, &tv);
            continue;
        }
        return false;
    }

    return true;
}

static bool recv_status_200(int fd)
{
    char resp[768];
    size_t got = 0;
    int64_t deadline = esp_timer_get_time() + 2000000;

    while (got + 1 < sizeof(resp)) {
        ssize_t n = recv(fd, resp + got, sizeof(resp) - got - 1, MSG_DONTWAIT);
        if (n > 0) {
            got += (size_t)n;
            resp[got] = '\0';
            if (strstr(resp, "\r\n"))
                break;
            continue;
        }

        if (n == 0)
            break;

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (esp_timer_get_time() > deadline)
                break;
            fd_set rfds;
            struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            (void)select(fd + 1, &rfds, NULL, NULL, &tv);
            continue;
        }
        break;
    }

    if (got == 0)
        return false;

    /* Need success status line first. */
    if (strstr(resp, " 200 ") == NULL)
        return false;

    /* Parse "id=<number>" from lobby response body (format: "status=OK\nid=N\n...") */
    lobby_room_id[0] = '\0';
    char *body = strstr(resp, "\r\n\r\n");
    if (body) {
        body += 4;
        /* Response must start with "status=OK" */
        char *id_tag = strstr(body, "id=");
        if (id_tag) {
            id_tag += 3; /* skip "id=" */
            size_t n = 0;
            while (id_tag[n] && id_tag[n] != '\r' && id_tag[n] != '\n'
                   && n + 1 < sizeof(lobby_room_id)) {
                lobby_room_id[n] = id_tag[n];
                n++;
            }
            lobby_room_id[n] = '\0';
        }
    }

    return true;
}

static void bytes_to_base64(const uint8_t *src, size_t len,
                           char *dst, size_t dst_len)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    size_t full_triples = len / 3;
    size_t remainder    = len % 3;
    size_t need = full_triples * 4 + (remainder ? 4 : 0) + 1;

    if (!dst || dst_len < need) {
        if (dst && dst_len) dst[0] = '\0';
        return;
    }

    for (i = 0; i < full_triples; i++) {
        uint32_t v = ((uint32_t)src[i*3] << 16) |
                     ((uint32_t)src[i*3+1] << 8) |
                     (uint32_t)src[i*3+2];
        dst[o++] = b64[(v >> 18) & 0x3f];
        dst[o++] = b64[(v >> 12) & 0x3f];
        dst[o++] = b64[(v >> 6)  & 0x3f];
        dst[o++] = b64[v & 0x3f];
    }

    if (remainder == 1) {
        uint32_t v = (uint32_t)src[i*3] << 16;
        dst[o++] = b64[(v >> 18) & 0x3f];
        dst[o++] = b64[(v >> 12) & 0x3f];
        dst[o++] = '=';
        dst[o++] = '=';
    } else if (remainder == 2) {
        uint32_t v = ((uint32_t)src[i*3] << 16) |
                     ((uint32_t)src[i*3+1] << 8);
        dst[o++] = b64[(v >> 18) & 0x3f];
        dst[o++] = b64[(v >> 12) & 0x3f];
        dst[o++] = b64[(v >> 6)  & 0x3f];
        dst[o++] = '=';
    }

    dst[o] = '\0';
}

static bool lobby_post_add(void)
{
    struct sockaddr_in addr;
    int fd;
    char encoded_nick[96];
    char encoded_password[96];
    char encoded_spectate_pw[96];
    size_t enc_off = 0;
    char body[896];
    char req[1152];

    if (!lobby_enabled())
        return false;

    if (!resolve_host_addr(gpsp_netplay_lobby_host, gpsp_netplay_lobby_port, &addr)) {
        ESP_LOGW(TAG, "Invalid lobby address: %s", gpsp_netplay_lobby_host);
        return false;
    }

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGW(TAG, "Lobby socket create failed: errno=%d", errno);
        return false;
    }

    set_nonblocking_nodelay(fd);
    int rc = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "Lobby connect failed immediately: errno=%d", errno);
        close(fd);
        return false;
    }
    if (!socket_connect_wait(fd, 2000)) {
        ESP_LOGW(TAG, "Lobby connect timeout/fail: %s:%u",
                 gpsp_netplay_lobby_host,
                 (unsigned)gpsp_netplay_lobby_port);
        close(fd);
        return false;
    }

    encoded_nick[0] = '\0';
    enc_off = 0;
    percent_encode_append(encoded_nick, sizeof(encoded_nick), &enc_off,
                          gpsp_netplay_ra_nick);

    encoded_password[0] = '\0';
    enc_off = 0;
    percent_encode_append(encoded_password, sizeof(encoded_password), &enc_off,
                          gpsp_netplay_host_password);

    encoded_spectate_pw[0] = '\0';
    enc_off = 0;
    percent_encode_append(encoded_spectate_pw, sizeof(encoded_spectate_pw), &enc_off,
                          gpsp_netplay_host_password);

    char encoded_frontend[64];
    encoded_frontend[0] = '\0';
    enc_off = 0;
    percent_encode_append(encoded_frontend, sizeof(encoded_frontend), &enc_off,
                          LOBBY_FRONTEND);

    char encoded_session[64];
    encoded_session[0] = '\0';
    enc_off = 0;
    percent_encode_append(encoded_session, sizeof(encoded_session), &enc_off,
                          ctrl_session_id);

    char encoded_relay[96];
    encoded_relay[0] = '\0';
    enc_off = 0;
    percent_encode_append(encoded_relay, sizeof(encoded_relay), &enc_off,
                          gpsp_netplay_ra_host);

    snprintf(body, sizeof(body),
             "username=%s"
             "&country=%s"
             "&core_name=%s"
             "&core_version=%s"
             "&game_name=%s"
             "&game_crc=%s"
             "&subsystem_name=%s"
             "&port=%u"
             "&force_mitm=1"
             "&mitm_server=custom"
             "&mitm_session=%s"
             "&mitm_custom_addr=%s"
             "&mitm_custom_port=%u"
             "&has_password=%d"
             "&has_spectate_password=%d"
             "&retroarch_version=%s"
             "&frontend=%s",
             encoded_nick,
             gpsp_netplay_lobby_country,
             LOBBY_CORE_NAME,
             LOBBY_CORE_VERSION,
             LOBBY_GAME_NAME,
             LOBBY_GAME_CRC,
             LOBBY_SUBSYSTEM,
             (unsigned)gpsp_netplay_ra_port,
             encoded_session,
             encoded_relay,
             (unsigned)gpsp_netplay_ra_port,
             gpsp_netplay_host_password[0] ? 1 : 0,
             gpsp_netplay_host_password[0] ? 1 : 0,
             LOBBY_RA_VERSION,
             encoded_frontend);

    snprintf(req, sizeof(req),
             "POST /add HTTP/1.1\r\n"
             "Host: %s:%u\r\n"
             "Connection: close\r\n"
             "Content-Type: application/x-www-form-urlencoded\r\n"
             "Content-Length: %u\r\n\r\n"
             "%s",
             gpsp_netplay_lobby_host,
             (unsigned)gpsp_netplay_lobby_port,
             (unsigned)strlen(body),
             body);

    bool ok = send_with_timeout(fd, req, strlen(req), 2000) && recv_status_200(fd);
    close(fd);

    if (ok) {
        ESP_LOGI(TAG, "Lobby publish OK: relay=%s session=%s lobby_room=%s",
                 gpsp_netplay_lobby_relay, ctrl_session_id,
                 lobby_room_id[0] ? lobby_room_id : "(n/a)");
    } else {
        ESP_LOGW(TAG, "Lobby publish failed");
    }
    return ok;
}

static void lobby_try_publish(bool force)
{
    if (!lobby_enabled() || ctrl_session_id[0] == '\0')
        return;

    int64_t now_us = esp_timer_get_time();
    if (!force && (now_us - lobby_last_post_us) < LOBBY_POST_INTERVAL_US)
        return;

    if (lobby_post_add()) {
        lobby_last_post_us = now_us;
    }
}

static void bytes_to_hex(const uint8_t *src, size_t len, char *dst, size_t dst_len)
{
    static const char hex[] = "0123456789abcdef";

    if (!dst || dst_len == 0)
        return;

    if (!src || dst_len < (len * 2 + 1)) {
        dst[0] = '\0';
        return;
    }

    for (size_t i = 0; i < len; i++) {
        dst[i * 2] = hex[src[i] >> 4];
        dst[i * 2 + 1] = hex[src[i] & 0x0f];
    }
    dst[len * 2] = '\0';
}

static void set_status(const char *status)
{
    strlcpy(ctrl_status, status ? status : "idle", sizeof(ctrl_status));
}

static bool set_nonblocking_nodelay(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return true;
}

static bool resolve_host_addr(const char *host, uint16_t port,
                              struct sockaddr_in *out_addr)
{
    if (!host || !host[0] || !out_addr)
        return false;

    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->sin_family = AF_INET;
    out_addr->sin_port = htons(port);

    if (inet_aton(host, &out_addr->sin_addr))
        return true;

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return false;

    memcpy(out_addr, res->ai_addr, sizeof(*out_addr));
    freeaddrinfo(res);
    return true;
}

static int start_socket_connect(void)
{
    struct sockaddr_in addr;

    if (!resolve_host_addr(gpsp_netplay_ra_host, gpsp_netplay_ra_port, &addr)) {
        ESP_LOGW(TAG, "Invalid tunnel relay address: %s", gpsp_netplay_ra_host);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGW(TAG, "Failed to create tunnel socket: errno=%d", errno);
        return -1;
    }

    set_nonblocking_nodelay(fd);

    int ret = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "Tunnel connect failed: errno=%d", errno);
        close(fd);
        return -1;
    }

    return fd;
}

static bool socket_connect_done(int fd)
{
    fd_set wfds;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    int err = 0;
    socklen_t err_len = sizeof(err);

    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);

    int ret = select(fd + 1, NULL, &wfds, NULL, &tv);
    if (ret <= 0)
        return false;

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0 || err != 0)
        return false;

    return true;
}

static bool socket_connect_wait(int fd, int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline) {
        if (socket_connect_done(fd))
            return true;

        fd_set wfds;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        (void)select(fd + 1, NULL, &wfds, NULL, &tv);
    }

    return socket_connect_done(fd);
}

static bool send_all_fd(int fd, const void *data, size_t len)
{
    const uint8_t *ptr = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, ptr + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return false;
        }
        sent += (size_t)n;
    }

    return true;
}

static void close_pending_link(pending_link_t *link)
{
    if (!link)
        return;

    if (link->state != LINK_SLOT_EMPTY && link->fd >= 0)
        close(link->fd);

    memset(link, 0, sizeof(*link));
    link->fd = -1;
}

static void stop_all(void)
{
    if (ctrl_fd >= 0) {
        close(ctrl_fd);
        ctrl_fd = -1;
    }

    for (int i = 0; i < TUNNEL_MAX_PENDING; i++)
        close_pending_link(&pending_links[i]);

    ctrl_state = TUNNEL_CTRL_DISCONNECTED;
    ctrl_recv_len = 0;
    ctrl_session_id[0] = '\0';
    lobby_room_id[0] = '\0';
    lobby_last_post_us = 0;
    set_status("idle");
}

static bool start_control_connect(void)
{
    int64_t now_us = esp_timer_get_time();

    if (now_us - ctrl_last_connect_attempt_us < 2000000)
        return false;
    ctrl_last_connect_attempt_us = now_us;

    if (!c6_remote_network_ready()) {
        set_status("waiting_net");
        return false;
    }

    if (gpsp_netplay_ra_host[0] == '\0') {
        set_status("need_host");
        return false;
    }

    ctrl_fd = start_socket_connect();
    if (ctrl_fd < 0) {
        set_status("connect_fail");
        return false;
    }

    ctrl_recv_len = 0;
    ctrl_state = TUNNEL_CTRL_CONNECTING;
    set_status("connecting");
    ESP_LOGI(TAG, "Connecting to tunnel relay %s:%u ...",
             gpsp_netplay_ra_host, gpsp_netplay_ra_port);
    return true;
}

static void drop_control(void)
{
    if (ctrl_fd >= 0) {
        close(ctrl_fd);
        ctrl_fd = -1;
    }
    ctrl_state = TUNNEL_CTRL_DISCONNECTED;
    ctrl_recv_len = 0;
    ctrl_shadow_len = 0;
    ctrl_shadow_closed = false;
    ctrl_session_id[0] = '\0';
    lobby_room_id[0] = '\0';
}

static bool recv_into_ctrl(void)
{
    if (ctrl_fd < 0 || ctrl_recv_len >= sizeof(ctrl_recv_buf))
        return false;

    if (ctrl_shadow_mutex && xSemaphoreTake(ctrl_shadow_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
        if (ctrl_shadow_closed) {
            ctrl_shadow_closed = false;
            xSemaphoreGive(ctrl_shadow_mutex);
            ESP_LOGW(TAG, "Tunnel control closed by relay");
            drop_control();
            set_status("closed");
            return false;
        }

        if (ctrl_shadow_len > 0) {
            size_t space = sizeof(ctrl_recv_buf) - ctrl_recv_len;
            size_t copy = ctrl_shadow_len < space ? ctrl_shadow_len : space;
            memcpy(ctrl_recv_buf + ctrl_recv_len, ctrl_shadow_buf, copy);
            ctrl_recv_len += copy;
            ctrl_shadow_len -= copy;
            if (ctrl_shadow_len > 0)
                memmove(ctrl_shadow_buf, ctrl_shadow_buf + copy, ctrl_shadow_len);
            xSemaphoreGive(ctrl_shadow_mutex);
            return true;
        }
        xSemaphoreGive(ctrl_shadow_mutex);
    }

    if (tunnel_io_task_handle)
        return false;

    ssize_t n = recv(ctrl_fd, ctrl_recv_buf + ctrl_recv_len,
                     sizeof(ctrl_recv_buf) - ctrl_recv_len, MSG_DONTWAIT);
    if (n > 0) {
        ctrl_recv_len += (size_t)n;
        return true;
    }

    if (n == 0) {
        ESP_LOGW(TAG, "Tunnel control closed by relay");
        drop_control();
        set_status("closed");
        return false;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "Tunnel control recv failed: errno=%d", errno);
        drop_control();
        set_status("recv_fail");
    }
    return false;
}

esp_err_t netpacket_tunnel_host_background_start(BaseType_t core_id, UBaseType_t priority)
{
    BaseType_t ret;

    if (tunnel_io_task_handle)
        return ESP_OK;

    if (!ctrl_shadow_mutex) {
        ctrl_shadow_mutex = xSemaphoreCreateMutex();
        if (!ctrl_shadow_mutex)
            return ESP_FAIL;
    }

    ret = xTaskCreatePinnedToCore(tunnel_ctrl_io_task, "np_tunnel_io", 2048,
                                  NULL, priority, &tunnel_io_task_handle, core_id);
    if (ret != pdPASS) {
        tunnel_io_task_handle = NULL;
        ESP_LOGW(TAG, "Failed to create tunnel IO task; fallback to sync recv");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void tunnel_ctrl_io_task(void *param)
{
    (void)param;
    uint8_t tmp[32];

    for (;;) {
        bool did_work = false;

        if (gpsp_netplay_ra_mode != NETPLAY_MODE_TUNNEL_HOST || ctrl_fd < 0) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        {
            size_t space = 0;
            size_t want = 0;
            ssize_t n;

            if (ctrl_shadow_mutex && xSemaphoreTake(ctrl_shadow_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
                space = sizeof(ctrl_shadow_buf) - ctrl_shadow_len;
                xSemaphoreGive(ctrl_shadow_mutex);
            }

            if (space == 0) {
                n = -1;
            } else {
                want = space < sizeof(tmp) ? space : sizeof(tmp);
                n = recv(ctrl_fd, tmp, want, MSG_DONTWAIT);
            }

            if (n > 0) {
                if (ctrl_shadow_mutex && xSemaphoreTake(ctrl_shadow_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
                    memcpy(ctrl_shadow_buf + ctrl_shadow_len, tmp, (size_t)n);
                    ctrl_shadow_len += (size_t)n;
                    did_work = true;
                    xSemaphoreGive(ctrl_shadow_mutex);
                }
            } else if (n == 0) {
                if (ctrl_shadow_mutex && xSemaphoreTake(ctrl_shadow_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
                    ctrl_shadow_closed = true;
                    xSemaphoreGive(ctrl_shadow_mutex);
                }
            }
        }

        if (!did_work)
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        else
            taskYIELD();
    }
}

static void ctrl_consume(size_t n)
{
    if (n >= ctrl_recv_len) {
        ctrl_recv_len = 0;
        return;
    }

    memmove(ctrl_recv_buf, ctrl_recv_buf + n, ctrl_recv_len - n);
    ctrl_recv_len -= n;
}

static void start_link_connect(const uint8_t peer_id[TUNNEL_UNIQUE_SIZE])
{
    pending_link_t *slot = NULL;

    for (int i = 0; i < TUNNEL_MAX_PENDING; i++) {
        if (pending_links[i].state != LINK_SLOT_EMPTY &&
            memcmp(pending_links[i].peer_id, peer_id, TUNNEL_UNIQUE_SIZE) == 0) {
            return;
        }
        if (!slot && pending_links[i].state == LINK_SLOT_EMPTY)
            slot = &pending_links[i];
    }

    if (!slot) {
        ESP_LOGW(TAG, "No free pending link slots for tunnel peer");
        return;
    }

    int fd = start_socket_connect();
    if (fd < 0)
        return;

    memset(slot, 0, sizeof(*slot));
    slot->fd = fd;
    slot->state = LINK_SLOT_CONNECTING;
    memcpy(slot->peer_id, peer_id, TUNNEL_UNIQUE_SIZE);
    bytes_to_hex(peer_id, TUNNEL_UNIQUE_SIZE, slot->peer_hex, sizeof(slot->peer_hex));
    ESP_LOGI(TAG, "Connecting tunnel peer link for %s", slot->peer_hex);
}

static void process_control_ready(void)
{
    while (ctrl_recv_len >= 4 && ctrl_state == TUNNEL_CTRL_READY) {
        if (memcmp(ctrl_recv_buf, TUNNEL_MAGIC_PING, 4) == 0) {
            ctrl_consume(4);
            if (!send_all_fd(ctrl_fd, TUNNEL_MAGIC_PING, 4)) {
                ESP_LOGW(TAG, "Failed to reply tunnel ping");
                drop_control();
                set_status("ping_fail");
                return;
            }
            continue;
        }

        if (memcmp(ctrl_recv_buf, TUNNEL_MAGIC_LINK, 4) == 0) {
            if (ctrl_recv_len < TUNNEL_MSG_SIZE)
                break;

            uint8_t peer_id[TUNNEL_UNIQUE_SIZE];
            memcpy(peer_id, ctrl_recv_buf + 4, sizeof(peer_id));
            ctrl_consume(TUNNEL_MSG_SIZE);
            start_link_connect(peer_id);
            continue;
        }

        ESP_LOGW(TAG, "Unknown tunnel control magic %.4s", ctrl_recv_buf);
        ctrl_consume(4);
    }
}

static void poll_pending_links(void)
{
    for (int i = 0; i < TUNNEL_MAX_PENDING; i++) {
        pending_link_t *link = &pending_links[i];
        if (link->state != LINK_SLOT_CONNECTING || link->fd < 0)
            continue;

        if (!socket_connect_done(link->fd))
            continue;

        uint8_t msg[TUNNEL_MSG_SIZE];
        memcpy(msg, TUNNEL_MAGIC_LINK, 4);
        memcpy(msg + 4, link->peer_id, TUNNEL_UNIQUE_SIZE);
        if (!send_all_fd(link->fd, msg, sizeof(msg))) {
            ESP_LOGW(TAG, "Failed to send tunnel link request for %s", link->peer_hex);
            close_pending_link(link);
            continue;
        }

        if (!netpacket_host_attach_client(link->fd, link->peer_hex)) {
            close_pending_link(link);
            continue;
        }

        ESP_LOGI(TAG, "Tunnel peer %s attached to host pipeline", link->peer_hex);
        memset(link, 0, sizeof(*link));
        link->fd = -1;
    }
}

void netpacket_tunnel_host_poll(void)
{
    if (gpsp_netplay_ra_mode != NETPLAY_MODE_TUNNEL_HOST) {
        if (ctrl_fd >= 0 || ctrl_state != TUNNEL_CTRL_DISCONNECTED)
            stop_all();
        return;
    }

    poll_pending_links();

    if (ctrl_state == TUNNEL_CTRL_DISCONNECTED) {
        start_control_connect();
        return;
    }

    if (ctrl_state == TUNNEL_CTRL_CONNECTING) {
        if (!socket_connect_done(ctrl_fd))
            return;

        uint8_t msg[TUNNEL_MSG_SIZE] = {0};
        memcpy(msg, TUNNEL_MAGIC_SESSION, 4);
        if (!send_all_fd(ctrl_fd, msg, sizeof(msg))) {
            ESP_LOGW(TAG, "Failed to request tunnel session");
            drop_control();
            set_status("session_req_fail");
            return;
        }

        ctrl_state = TUNNEL_CTRL_WAIT_SESSION;
        set_status("creating_room");
        return;
    }

    recv_into_ctrl();

    if (ctrl_state == TUNNEL_CTRL_WAIT_SESSION) {
        if (ctrl_recv_len < TUNNEL_MSG_SIZE)
            return;

        if (memcmp(ctrl_recv_buf, TUNNEL_MAGIC_SESSION, 4) != 0) {
            ESP_LOGW(TAG, "Unexpected tunnel session reply");
            drop_control();
            set_status("bad_reply");
            return;
        }

        bytes_to_base64(ctrl_recv_buf + 4, TUNNEL_UNIQUE_SIZE,
                        ctrl_session_id, sizeof(ctrl_session_id));
        ctrl_consume(TUNNEL_MSG_SIZE);
        ctrl_state = TUNNEL_CTRL_READY;
        set_status("room_ready");
        ESP_LOGI(TAG, "Tunnel session created: %s", ctrl_session_id);
        lobby_try_publish(true);
        return;
    }

    lobby_try_publish(false);
    process_control_ready();
}

const char *netpacket_tunnel_host_session_id(void)
{
    return ctrl_session_id;
}

const char *netpacket_tunnel_host_room_id(void)
{
    return lobby_room_id;
}

const char *netpacket_tunnel_host_status(void)
{
    return ctrl_status;
}

bool netpacket_tunnel_host_room_ready(void)
{
    return ctrl_state == TUNNEL_CTRL_READY && ctrl_session_id[0] != '\0';
}

bool netpacket_tunnel_host_has_pending_io(void)
{
    bool pending = false;

    /* Fallback path: without async IO task we cannot know pending status,
     * so report busy to preserve previous synchronous behavior. */
    if (!tunnel_io_task_handle)
        return true;

    if (!ctrl_shadow_mutex)
        return false;

    if (xSemaphoreTake(ctrl_shadow_mutex, 0) != pdTRUE)
        return true;

    pending = (ctrl_shadow_len > 0 || ctrl_shadow_closed);
    xSemaphoreGive(ctrl_shadow_mutex);
    return pending;
}