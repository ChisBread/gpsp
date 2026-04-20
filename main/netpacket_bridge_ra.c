/*
 * gpsp app support — RetroArch netplay TCP client for ESP32-P4
 *
 * Implements the RetroArch Netplay Core Packet Interface protocol
 * as a TCP client, allowing ESP32-P4 to join a RetroArch host running
 * the gpSP core for multiplayer gameplay.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "c6_remote.h"
#include "common.h"
#include "gpsp_config.h"
#include "main.h"
#include "netpacket_queue.h"
#include "netpacket_tunnel_host.h"
#include "ra_protocol.h"
#include "runtime_config.h"
#include "serial.h"

/* Host mode (netpacket_host.c) */
extern void netpacket_host_poll(void);
extern void netpacket_host_send(uint16_t client_id, const void *buf, size_t len);
extern size_t netpacket_host_flush_queued(void);
extern bool netpacket_host_has_pending_io(void);
extern esp_err_t netpacket_host_background_start(BaseType_t core_id, UBaseType_t priority);
extern void netpacket_host_notify_io_task(void);
extern esp_err_t netpacket_tunnel_host_background_start(BaseType_t core_id, UBaseType_t priority);

/* ── Connection states ────────────────────────────────────────────── */

typedef enum {
    STATE_DISCONNECTED = 0,
    STATE_CONNECTING,
    STATE_WAIT_SERVER_HEADER,
    STATE_SEND_NICK,
    STATE_WAIT_SERVER_NICK,
    STATE_SEND_INFO,
    STATE_WAIT_SERVER_INFO,
    STATE_WAIT_SYNC,
    STATE_SEND_PLAY,
    STATE_CONNECTED,
} netpacket_state_t;

/* ── Module state ─────────────────────────────────────────────────── */

u32 netplay_num_clients = 0;
u32 netplay_client_id = 0;

static const char *TAG = "gpsp_netpacket";
static int np_socket_fd = -1;
static netpacket_state_t np_state = STATE_DISCONNECTED;
static int64_t np_last_connect_attempt_us;
static int64_t np_last_ping_us;
#define NP_HOST_POLL_BUSY_INTERVAL_US 1000
#define NP_HOST_POLL_IDLE_INTERVAL_US 4000
static int64_t np_last_host_poll_us;
/* Rate-limit actual recv() syscalls: once per ~1 ms max.
 * rfu_update() calls netpacket_poll_receive() at every update_gba() boundary
 * (~456x/frame). Without throttling, 456 lwIP recv() calls/frame eat ~10ms.
 * SERVICE_CORE recv task calls recv() every ~1ms and writes to np_shadow_buf.
 * The emulation core's np_recv_more() drains np_shadow_buf under a mutex —
 * no lwIP recv() syscall on the emulation core at all. */
static uint32_t np_server_protocol;
static uint32_t np_password_salt;

typedef struct {
    int mode;
    uint16_t port;
    char host[sizeof(gpsp_netplay_ra_host)];
    char tunnel_id[sizeof(gpsp_netplay_ra_tunnel_id)];
    char nick[sizeof(gpsp_netplay_ra_nick)];
    char password[sizeof(gpsp_netplay_client_password)];
} np_client_cfg_t;

static np_client_cfg_t np_client_cfg_applied;
static bool np_client_cfg_valid;

/* Debug counters */
static uint32_t np_tx_packets, np_tx_bytes;
static uint32_t np_rx_packets, np_rx_bytes;

/* Receive buffer for TCP stream reassembly (emulation core only) */
#define NP_RECV_BUF_SIZE 4096
static GPSP_EXTRAM_BSS uint8_t np_recv_buf[NP_RECV_BUF_SIZE];
static size_t np_recv_len;

/* Shadow receive buffer: filled by np_recv_task (SERVICE_CORE) via recv(),
 * drained by np_recv_more() (emulation core) under np_shadow_mutex. */
#define NP_SHADOW_BUF_SIZE 4096
static GPSP_EXTRAM_BSS uint8_t np_shadow_buf[NP_SHADOW_BUF_SIZE];
static size_t np_shadow_len;
static SemaphoreHandle_t np_shadow_mutex;
static TaskHandle_t np_recv_task_handle = NULL;

static netpacket_queue_t np_send_queue;
static uint8_t *np_send_queue_storage;
static TaskHandle_t np_flush_task_handle = NULL;

static void np_flush_task(void *param);
static void np_shadow_recv_task(void *param);
void netpacket_notify_flush_task(void);
static bool np_ensure_send_queue(void);
static void np_free_send_queue(void);
static bool np_queue_packet_wait(const void *part1, size_t part1_len,
                                 const void *part2, size_t part2_len);
static bool np_queue_control_packet(const void *data, size_t len);

static bool np_mode_is_client(int mode)
{
    return (mode == NETPLAY_MODE_CLIENT || mode == NETPLAY_MODE_TUNNEL_CLIENT);
}

static void np_client_cfg_snapshot(np_client_cfg_t *cfg)
{
    cfg->mode = gpsp_netplay_ra_mode;
    cfg->port = gpsp_netplay_ra_port;
    strlcpy(cfg->host, gpsp_netplay_ra_host, sizeof(cfg->host));
    strlcpy(cfg->tunnel_id, gpsp_netplay_ra_tunnel_id, sizeof(cfg->tunnel_id));
    strlcpy(cfg->nick, gpsp_netplay_ra_nick, sizeof(cfg->nick));
    strlcpy(cfg->password, gpsp_netplay_client_password, sizeof(cfg->password));
}

static bool np_client_cfg_equals(const np_client_cfg_t *a, const np_client_cfg_t *b)
{
    return a->mode == b->mode &&
           a->port == b->port &&
           strcmp(a->host, b->host) == 0 &&
           strcmp(a->tunnel_id, b->tunnel_id) == 0 &&
           strcmp(a->nick, b->nick) == 0 &&
           strcmp(a->password, b->password) == 0;
}
/* ── Helpers ──────────────────────────────────────────────────────── */

static int base64_decode_char(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool np_parse_tunnel_id_base64(const char *b64, uint8_t out[12])
{
    /* 12 bytes = 16 base64 chars (no padding needed, 12 is multiple of 3) */
    size_t len = strlen(b64);
    /* Accept with or without trailing '=' padding */
    while (len > 0 && b64[len - 1] == '=') len--;
    if (len != 16) return false;

    for (size_t i = 0; i < 4; i++) {
        int a = base64_decode_char(b64[i * 4]);
        int b = base64_decode_char(b64[i * 4 + 1]);
        int c = base64_decode_char(b64[i * 4 + 2]);
        int d = base64_decode_char(b64[i * 4 + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;
        out[i * 3]     = (uint8_t)((a << 2) | (b >> 4));
        out[i * 3 + 1] = (uint8_t)(((b & 0x0f) << 4) | (c >> 2));
        out[i * 3 + 2] = (uint8_t)(((c & 0x03) << 6) | d);
    }
    return true;
}

static bool np_parse_tunnel_id_hex(const char *hex, uint8_t out[12])
{
    size_t len = strlen(hex);
    if (len != 24) return false;
    for (int i = 0; i < 12; i++) {
        char h[3] = { hex[i*2], hex[i*2+1], '\0' };
        char *end;
        unsigned long v = strtoul(h, &end, 16);
        if (*end != '\0') return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

/* Auto-detect hex (24 chars) or base64 (16 chars) tunnel session id */
static bool np_parse_tunnel_id(const char *id, uint8_t out[12])
{
    if (!id || !id[0]) return false;
    size_t len = strlen(id);
    /* Strip trailing '=' for length check */
    size_t stripped = len;
    while (stripped > 0 && id[stripped - 1] == '=') stripped--;
    if (len == 24 && np_parse_tunnel_id_hex(id, out))
        return true;
    if (stripped == 16 && np_parse_tunnel_id_base64(id, out))
        return true;
    return false;
}

static void np_disconnect(void)
{
    if (np_socket_fd >= 0) {
        close(np_socket_fd);
        np_socket_fd = -1;
    }
    np_state = STATE_DISCONNECTED;
    np_recv_len = 0;
    np_password_salt = 0;
    netplay_num_clients = 0;
    netplay_client_id = 0;
    np_free_send_queue();

    /* Clear shadow buffer so the recv task doesn't replay stale data */
    if (np_shadow_mutex && xSemaphoreTake(np_shadow_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        np_shadow_len = 0;
        xSemaphoreGive(np_shadow_mutex);
    }

    /* Reset serial protocol state machines to avoid stale peer state */
    serialproto_reset();
    rfu_reset();
    serial_reset_irq();
}

void netpacket_notify_flush_task(void)
{
    if (np_flush_task_handle)
        xTaskNotifyGive(np_flush_task_handle);
}

static bool np_ensure_send_queue(void)
{
    if (np_send_queue_storage)
        return true;

    np_send_queue_storage = heap_caps_malloc(NETPACKET_SEND_QUEUE_CAPACITY,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!np_send_queue_storage) {
        ESP_LOGE(TAG, "Failed to allocate client send queue (%u bytes)",
                 (unsigned)NETPACKET_SEND_QUEUE_CAPACITY);
        return false;
    }

    netpacket_queue_init(&np_send_queue, np_send_queue_storage,
                         NETPACKET_SEND_QUEUE_CAPACITY);
    return true;
}

static void np_free_send_queue(void)
{
    if (!np_send_queue_storage)
        return;

    heap_caps_free(np_send_queue_storage);
    np_send_queue_storage = NULL;
    netpacket_queue_init(&np_send_queue, NULL, 0);
}

static bool np_queue_packet_wait(const void *part1, size_t part1_len,
                                 const void *part2, size_t part2_len)
{
    if (!np_ensure_send_queue())
        return false;

    while (np_state == STATE_CONNECTED && np_socket_fd >= 0) {
        if (netpacket_queue_enqueue2(&np_send_queue, part1, part1_len,
                                     part2, part2_len)) {
            netpacket_notify_flush_task();
            return true;
        }

        netpacket_notify_flush_task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return false;
}

static bool np_queue_control_packet(const void *data, size_t len)
{
    return np_queue_packet_wait(data, len, NULL, 0);
}

static size_t np_flush_pending(void)
{
    uint8_t *data;
    size_t len;
    size_t total_flushed = 0;

    if (np_socket_fd < 0 || np_state != STATE_CONNECTED)
        return 0;

    while ((len = netpacket_queue_peek_contiguous(&np_send_queue, &data)) > 0) {
        ssize_t n = send(np_socket_fd, data, len, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            ESP_LOGW(TAG, "TCP send failed: errno=%d", errno);
            np_disconnect();
            break;
        }

        if (n == 0)
            break;

        netpacket_queue_consume(&np_send_queue, (size_t)n);
        total_flushed += (size_t)n;
    }

    return total_flushed;
}

static void np_flush_task(void *param)
{
    (void)param;

    for (;;) {
        bool did_work = false;

        if (gpsp_netplay_ra_mode == NETPLAY_MODE_DISABLED) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (c6_remote_network_ready()) {
            did_work = (np_flush_pending() != 0);
            if (netpacket_host_flush_queued() != 0)
                did_work = true;
        }

        if (did_work) {
            /* Yield briefly, then continue draining if more data arrives. */
            taskYIELD();
            continue;
        }

        /* Idle: sleep until a producer notifies us (every enqueue does).
         * Cap wait at 50 ms so we still detect mode toggles promptly. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
}

esp_err_t netpacket_background_start(BaseType_t core_id, UBaseType_t priority)
{
    BaseType_t ret;

    if (np_flush_task_handle)
        return ESP_OK;

    /* Shadow mutex — created once, never destroyed */
    if (!np_shadow_mutex) {
        np_shadow_mutex = xSemaphoreCreateMutex();
        if (!np_shadow_mutex) {
            ESP_LOGE(TAG, "Failed to create shadow recv mutex");
            return ESP_FAIL;
        }
    }

    ret = xTaskCreatePinnedToCore(np_flush_task, "np_flush", 4096, NULL,
                                  priority, &np_flush_task_handle, core_id);
    if (ret != pdPASS) {
        np_flush_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create netpacket flush task");
        return ESP_FAIL;
    }

    /* Recv task — same core as flush/WiFi, slightly lower priority */
    ret = xTaskCreatePinnedToCore(np_shadow_recv_task, "np_recv", 2048, NULL,
                                  priority - 1, &np_recv_task_handle, core_id);
    if (ret != pdPASS) {
        np_recv_task_handle = NULL;
        ESP_LOGW(TAG, "Failed to create netpacket recv task — falling back to throttled poll");
        /* Non-fatal: emulation core will call np_recv_more() without shadow */
    }

    /* Host/tunnel host RX+accept offload tasks (best-effort). */
    (void)netpacket_host_background_start(core_id, priority - 1);
    (void)netpacket_tunnel_host_background_start(core_id, priority - 1);

    return ESP_OK;
}

/* Non-blocking read into np_recv_buf. Returns bytes available. */
/* Drain shadow buffer (filled by np_recv_task on SERVICE_CORE) into
 * np_recv_buf.  Never calls recv() directly — no lwIP syscall cost. */
static ssize_t np_recv_more(void)
{
    if (np_recv_len >= NP_RECV_BUF_SIZE || !np_shadow_mutex)
        return (ssize_t)np_recv_len;

    if (xSemaphoreTake(np_shadow_mutex, 0) == pdTRUE) {
        if (np_shadow_len > 0) {
            size_t space = NP_RECV_BUF_SIZE - np_recv_len;
            size_t copy  = np_shadow_len < space ? np_shadow_len : space;
            memcpy(np_recv_buf + np_recv_len, np_shadow_buf, copy);
            np_recv_len  += copy;
            np_shadow_len -= copy;
            if (np_shadow_len > 0)
                memmove(np_shadow_buf, np_shadow_buf + copy, np_shadow_len);
        }
        xSemaphoreGive(np_shadow_mutex);
    }
    return (ssize_t)np_recv_len;
}

/* Background recv task — runs on SERVICE_CORE (same as WiFi).
 * Blocks in select() on the TCP socket so it only wakes when data is
 * available (or the emulation thread signals a state change). */
static void np_shadow_recv_task(void *param)
{
    (void)param;
    uint8_t tmp[512];

    for (;;) {
        if (gpsp_netplay_ra_mode == NETPLAY_MODE_DISABLED) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        int fd = np_socket_fd;
        if (fd < 0) {
            /* No socket — wait for connect to notify us.  Cap at 100 ms so
             * we refresh config/mode without spinning. */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        /* 20 ms cap lets us notice fd/mode changes promptly without busy
         * polling.  lwIP wakes select() as soon as bytes arrive. */
        struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
        int sel = select(fd + 1, &rfds, NULL, NULL, &tv);

        /* Re-check fd — it may have been closed while we waited. */
        if (np_socket_fd != fd)
            continue;
        if (sel <= 0)
            continue;

        ssize_t n = recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n <= 0)
            continue;

        if (xSemaphoreTake(np_shadow_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
            size_t space = NP_SHADOW_BUF_SIZE - np_shadow_len;
            size_t copy  = (size_t)n < space ? (size_t)n : space;
            memcpy(np_shadow_buf + np_shadow_len, tmp, copy);
            np_shadow_len += copy;
            xSemaphoreGive(np_shadow_mutex);
        }
    }
}

/* Blocking send for control messages (handshake, ping).
 * Tears down the connection on failure. */
static bool np_send_all(const void *data, size_t len)
{
    if (ra_send_all(np_socket_fd, data, len))
        return true;
    ESP_LOGW(TAG, "TCP send failed: errno=%d", errno);
    np_disconnect();
    return false;
}

/* Consume n bytes from front of np_recv_buf */
static void np_recv_consume(size_t n)
{
    ra_buf_consume(np_recv_buf, &np_recv_len, n);
}

/* Try to read exactly `needed` bytes into `out`. Returns true if available. */
static bool np_try_read(void *out, size_t needed)
{
    if (np_recv_len < needed) {
        if (np_recv_more() < 0) {
            return false;
        }
    }
    if (np_recv_len < needed) {
        return false;
    }
    memcpy(out, np_recv_buf, needed);
    np_recv_consume(needed);
    return true;
}

/* ── Receive dispatch ─────────────────────────────────────────────── */

static inline void np_receive_dispatch(const void *buf, size_t len, uint16_t client_id)
{
    ra_receive_dispatch(buf, len, client_id);
}

/* ── Handshake phases ─────────────────────────────────────────────── */

static bool np_start_connect(void)
{
    int sockfd;
    struct sockaddr_in server_addr;
    int64_t now_us = esp_timer_get_time();

    /* Rate-limit connection attempts to 2 sec */
    if (now_us - np_last_connect_attempt_us < 2000000) {
        return false;
    }
    np_last_connect_attempt_us = now_us;

    /* Host modes use host-side handlers, not outbound RA client connection */
    if (gpsp_netplay_ra_mode == NETPLAY_MODE_HOST ||
        gpsp_netplay_ra_mode == NETPLAY_MODE_TUNNEL_HOST) {
        return false;
    }

    if (!c6_remote_network_ready()) {
        ESP_LOGD(TAG, "Network not ready, deferring connect");
        return false;
    }

    if (gpsp_netplay_ra_host[0] == '\0') {
        ESP_LOGD(TAG, "No host configured");
        return false;
    }

    if (gpsp_netplay_ra_mode == NETPLAY_MODE_TUNNEL_CLIENT &&
        gpsp_netplay_ra_tunnel_id[0] == '\0') {
        ESP_LOGD(TAG, "No tunnel_id configured");
        return false;
    }

    if (!ra_resolve_host(gpsp_netplay_ra_host, gpsp_netplay_ra_port, &server_addr)) {
        ESP_LOGW(TAG, "Failed to resolve host: %s", gpsp_netplay_ra_host);
        return false;
    }

    if (!np_ensure_send_queue()) {
        return false;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        ESP_LOGW(TAG, "Failed to create TCP socket: errno=%d", errno);
        return false;
    }

    ra_socket_prepare(sockfd);

    ESP_LOGI(TAG, "Connecting to RetroArch host %s:%u ...",
             gpsp_netplay_ra_host, gpsp_netplay_ra_port);

    int ret = connect(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "TCP connect failed: errno=%d", errno);
        close(sockfd);
        return false;
    }

    np_socket_fd = sockfd;
    np_recv_len = 0;
    np_state = STATE_CONNECTING;
    /* Wake recv task so its select() re-arms on the new fd. */
    if (np_recv_task_handle)
        xTaskNotifyGive(np_recv_task_handle);
    return true;
}

static bool np_check_connect(void)
{
    fd_set wfds;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    int err = 0;
    socklen_t err_len = sizeof(err);

    FD_ZERO(&wfds);
    FD_SET(np_socket_fd, &wfds);

    int ret = select(np_socket_fd + 1, NULL, &wfds, NULL, &tv);
    if (ret <= 0) {
        return false;  /* Still connecting */
    }

    if (getsockopt(np_socket_fd, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0 || err != 0) {
        ESP_LOGW(TAG, "TCP connect error: %d", err);
        np_disconnect();
        return false;
    }

    ESP_LOGI(TAG, "TCP connected to %s",
             gpsp_netplay_ra_mode == NETPLAY_MODE_TUNNEL_CLIENT
                 ? "tunnel server" : "RetroArch host");

    /* For tunnel client, send RATS + session_id before RANP header */
    if (gpsp_netplay_ra_mode == NETPLAY_MODE_TUNNEL_CLIENT) {
        uint8_t rats_msg[16];
        uint8_t session_bytes[12];
        if (!np_parse_tunnel_id(gpsp_netplay_ra_tunnel_id, session_bytes)) {
            ESP_LOGE(TAG, "Invalid tunnel_id (need 24-char hex or 16-char base64): %s",
                     gpsp_netplay_ra_tunnel_id);
            np_disconnect();
            return false;
        }
        memcpy(rats_msg, "RATS", 4);
        memcpy(rats_msg + 4, session_bytes, 12);
        if (!ra_send_all(np_socket_fd, rats_msg, sizeof(rats_msg))) {
            np_disconnect();
            return false;
        }
        ESP_LOGI(TAG, "Sent tunnel RATS: session=%s", gpsp_netplay_ra_tunnel_id);
    }

    /* Send our header immediately */
    uint32_t header[6];
    header[0] = htonl(RA_NETPLAY_MAGIC);
    header[1] = htonl(ra_platform_magic());
    header[2] = htonl(0);  /* No compression */
    header[3] = htonl(RA_NETPLAY_PROTOCOL_VERSION);  /* Highest supported */
    header[4] = htonl(0);  /* Negotiate */
    header[5] = htonl(ra_impl_magic());

    if (!np_send_all(header, sizeof(header))) {
        return false;
    }

    np_state = STATE_WAIT_SERVER_HEADER;
    return true;
}

static bool np_handle_server_header(void)
{
    uint32_t header[6];

    if (!np_try_read(header, sizeof(header))) {
        return false;
    }

    if (ntohl(header[0]) != RA_NETPLAY_MAGIC) {
        ESP_LOGE(TAG, "Bad magic from server: 0x%08x", ntohl(header[0]));
        np_disconnect();
        return false;
    }

    np_server_protocol = ntohl(header[4]);
    if (np_server_protocol < 5 || np_server_protocol > RA_NETPLAY_PROTOCOL_VERSION) {
        ESP_LOGE(TAG, "Unsupported protocol version: %u", (unsigned)np_server_protocol);
        np_disconnect();
        return false;
    }

    uint32_t salt = ntohl(header[3]);
    np_password_salt = 0;
    if (salt != 0) {
        if (gpsp_netplay_client_password[0] == '\0') {
            ESP_LOGE(TAG, "Server requires password but none configured");
            np_disconnect();
            return false;
        }

        np_password_salt = salt;
    }

    ESP_LOGI(TAG, "Server protocol version: %u", (unsigned)np_server_protocol);

    /* Send NICK */
    np_state = STATE_SEND_NICK;
    return true;
}

static bool np_send_nick(void)
{
    uint32_t cmd[2];
    char nick[RA_NICK_LEN];

    memset(nick, 0, sizeof(nick));
    strlcpy(nick, gpsp_netplay_ra_nick, sizeof(nick));

    cmd[0] = htonl(RA_CMD_NICK);
    cmd[1] = htonl(RA_NICK_LEN);

    ESP_LOGI(TAG, "Sending NICK: \"%s\"", nick);

    if (!np_send_all(cmd, sizeof(cmd)) || !np_send_all(nick, sizeof(nick))) {
        return false;
    }

    if (np_password_salt != 0) {
        /* RetroArch order: send NICK first, then NETPLAY_CMD_PASSWORD. */
        uint32_t pass_cmd[2];
        char hash[RA_PASS_HASH_LEN + 1];

        ra_password_hash_hex(np_password_salt, gpsp_netplay_client_password, hash);

        pass_cmd[0] = htonl(RA_CMD_PASSWORD);
        pass_cmd[1] = htonl(RA_PASS_HASH_LEN);

        if (!np_send_all(pass_cmd, sizeof(pass_cmd)) ||
            !np_send_all(hash, RA_PASS_HASH_LEN)) {
            np_disconnect();
            return false;
        }
        ESP_LOGI(TAG, "Password hash sent");
    }

    np_state = STATE_WAIT_SERVER_NICK;
    return true;
}

static bool np_handle_server_nick(void)
{
    uint32_t cmd[2];
    char nick[RA_NICK_LEN];

    if (!np_try_read(cmd, sizeof(cmd))) {
        return false;
    }

    if (ntohl(cmd[0]) != RA_CMD_NICK || ntohl(cmd[1]) != RA_NICK_LEN) {
        ESP_LOGE(TAG, "Expected NICK from server, got cmd=0x%04x size=%u",
                 ntohl(cmd[0]), ntohl(cmd[1]));
        np_disconnect();
        return false;
    }

    if (!np_try_read(nick, sizeof(nick))) {
        /* Put cmd header back — need to re-read everything next time.
           This won't happen often since nick follows immediately. */
        np_disconnect();
        return false;
    }

    nick[RA_NICK_LEN - 1] = '\0';
    ESP_LOGI(TAG, "Server nick: \"%s\"", nick);

    np_state = STATE_SEND_INFO;
    return true;
}

static bool np_send_info(void)
{
    struct {
        uint32_t cmd[2];
        uint32_t content_crc;
        char core_name[32];
        char core_version[32];
    } __attribute__((packed)) info = {0};

    info.cmd[0] = htonl(RA_CMD_INFO);
    info.cmd[1] = htonl(sizeof(info) - sizeof(info.cmd));
    info.content_crc = htonl(0);  /* CRC not critical for CORE_PACKET_INTERFACE */
    strlcpy(info.core_name, GPSP_NAME, sizeof(info.core_name));
    strlcpy(info.core_version, GPSP_NETPACKET_VERSION, sizeof(info.core_version));

    ESP_LOGI(TAG, "Sending INFO: core=%s ver=%s crc=0", info.core_name, info.core_version);

    if (!np_send_all(&info, sizeof(info))) {
        return false;
    }

    np_state = STATE_WAIT_SERVER_INFO;
    return true;
}

static bool np_handle_server_info(void)
{
    uint32_t cmd[2];
    uint32_t content_crc;
    char core_name[32];
    char core_version[32];

    if (!np_try_read(cmd, sizeof(cmd))) {
        return false;
    }

    uint32_t cmd_id = ntohl(cmd[0]);
    uint32_t cmd_size = ntohl(cmd[1]);

    if (cmd_id != RA_CMD_INFO) {
        ESP_LOGE(TAG, "Expected INFO from server, got cmd=0x%04x", cmd_id);
        np_disconnect();
        return false;
    }

    if (cmd_size < sizeof(content_crc) + sizeof(core_name) + sizeof(core_version)) {
        ESP_LOGE(TAG, "INFO payload too small: %u", (unsigned)cmd_size);
        np_disconnect();
        return false;
    }

    if (!np_try_read(&content_crc, sizeof(content_crc)) ||
        !np_try_read(core_name, sizeof(core_name)) ||
        !np_try_read(core_version, sizeof(core_version))) {
        np_disconnect();
        return false;
    }

    /* Consume any extra bytes in the INFO payload */
    size_t extra = cmd_size - sizeof(content_crc) - sizeof(core_name) - sizeof(core_version);
    while (extra > 0) {
        uint8_t discard[64];
        size_t chunk = extra < sizeof(discard) ? extra : sizeof(discard);
        if (!np_try_read(discard, chunk)) {
            np_disconnect();
            return false;
        }
        extra -= chunk;
    }

    core_name[31] = '\0';
    core_version[31] = '\0';
    ESP_LOGI(TAG, "Server core: %s %s, CRC: 0x%08x",
             core_name, core_version, ntohl(content_crc));

    np_state = STATE_WAIT_SYNC;
    return true;
}

static bool np_handle_sync(void)
{
    uint32_t cmd[2];
    uint32_t frame_count, client_num;

    if (!np_try_read(cmd, sizeof(cmd))) {
        return false;
    }

    uint32_t cmd_id = ntohl(cmd[0]);
    uint32_t cmd_size = ntohl(cmd[1]);

    if (cmd_id != RA_CMD_SYNC) {
        /* Could be a SETTING command that arrived before SYNC, skip it */
        ESP_LOGD(TAG, "Pre-SYNC cmd 0x%04x size %u, skipping", cmd_id, (unsigned)cmd_size);
        while (cmd_size > 0) {
            uint8_t discard[256];
            size_t chunk = cmd_size < sizeof(discard) ? cmd_size : sizeof(discard);
            if (!np_try_read(discard, chunk)) {
                np_disconnect();
                return false;
            }
            cmd_size -= chunk;
        }
        return false;  /* Try again next poll */
    }

    /* Read frame_count and client_num */
    if (!np_try_read(&frame_count, sizeof(frame_count)) ||
        !np_try_read(&client_num, sizeof(client_num))) {
        np_disconnect();
        return false;
    }

    client_num = ntohl(client_num);
    if (client_num & RA_SYNC_BIT_PAUSED) {
        client_num ^= RA_SYNC_BIT_PAUSED;
    }

    /* Read device configs (16 × uint32_t) */
    uint32_t devices[RA_MAX_INPUT_DEVICES];
    if (!np_try_read(devices, sizeof(devices))) {
        np_disconnect();
        return false;
    }

    /* Read share modes (16 × uint8_t) */
    uint8_t share_modes[RA_MAX_INPUT_DEVICES];
    if (!np_try_read(share_modes, sizeof(share_modes))) {
        np_disconnect();
        return false;
    }

    /* Read device-client mapping (16 × uint32_t) */
    uint32_t device_clients[RA_MAX_INPUT_DEVICES];
    if (!np_try_read(device_clients, sizeof(device_clients))) {
        np_disconnect();
        return false;
    }

    /* Read nick (32 bytes) */
    char nick[RA_NICK_LEN];
    if (!np_try_read(nick, sizeof(nick))) {
        np_disconnect();
        return false;
    }

    /* In CORE_PACKET_INTERFACE mode, SRAM size should be 0.
       Calculate remaining bytes and consume them (if any). */
    size_t sync_fixed = 2 * sizeof(uint32_t)
        + RA_MAX_INPUT_DEVICES * sizeof(uint32_t)
        + RA_MAX_INPUT_DEVICES * sizeof(uint8_t)
        + RA_MAX_INPUT_DEVICES * sizeof(uint32_t)
        + RA_NICK_LEN;

    if (cmd_size > sync_fixed) {
        size_t sram_size = cmd_size - sync_fixed;
        ESP_LOGW(TAG, "Unexpected SRAM in SYNC (%u bytes), discarding", (unsigned)sram_size);
        while (sram_size > 0) {
            uint8_t discard[256];
            size_t chunk = sram_size < sizeof(discard) ? sram_size : sizeof(discard);
            if (!np_try_read(discard, chunk)) {
                np_disconnect();
                return false;
            }
            sram_size -= chunk;
        }
    }

    netplay_client_id = client_num;
    nick[RA_NICK_LEN - 1] = '\0';
    ESP_LOGI(TAG, "SYNC received: client_id=%u, nick=\"%s\"",
             (unsigned)client_num, nick);

    np_state = STATE_SEND_PLAY;
    return true;
}

static bool np_send_play(void)
{
    /* First, drain any post-SYNC SETTING commands */
    while (np_recv_len >= 8) {
        uint32_t peek_cmd[2];
        memcpy(peek_cmd, np_recv_buf, sizeof(peek_cmd));
        uint32_t cmd_id = ntohl(peek_cmd[0]);
        uint32_t cmd_size = ntohl(peek_cmd[1]);

        if (cmd_id == RA_CMD_SETTING_ALLOW_PAUSE ||
            cmd_id == RA_CMD_SETTING_INPUT_LATENCY) {
            size_t total = 8 + cmd_size;
            if (np_recv_len < total) {
                break;  /* Not enough data yet */
            }
            ESP_LOGD(TAG, "Draining post-SYNC setting cmd 0x%04x", cmd_id);
            np_recv_consume(total);
            continue;
        }
        break;
    }

    uint32_t play_cmd[3];
    play_cmd[0] = htonl(RA_CMD_PLAY);
    play_cmd[1] = htonl(4);
    play_cmd[2] = htonl(0);  /* No devices, no slave, no share mode */

    if (!np_send_all(play_cmd, sizeof(play_cmd))) {
        return false;
    }

    np_state = STATE_CONNECTED;
    np_last_ping_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Connected! client_id=%u, entering PLAYING mode",
             (unsigned)netplay_client_id);
    return true;
}

/* ── Runtime command processing ───────────────────────────────────── */

static void np_process_commands(void)
{
    while (np_recv_len >= 8 && np_state == STATE_CONNECTED) {
        uint32_t cmd_hdr[2];
        memcpy(cmd_hdr, np_recv_buf, sizeof(cmd_hdr));
        uint32_t cmd_id = ntohl(cmd_hdr[0]);
        uint32_t cmd_size = ntohl(cmd_hdr[1]);

        /*
         * NETPACKET wire format: cmd(4) + size(4) + client_id(4) + data(size)
         * All other commands:    cmd(4) + size(4) + data(size)
         *
         * For NETPACKET, the client_id field is NOT included in cmd_size.
         * Total bytes after header = 4 + cmd_size for NETPACKET,
         *                          = cmd_size for all other commands.
         */
        size_t total_needed;
        if (cmd_id == RA_CMD_NETPACKET) {
            total_needed = 8 + 4 + cmd_size;  /* hdr + client_id + data */
        } else {
            total_needed = 8 + cmd_size;       /* hdr + data */
        }

        if (np_recv_len < total_needed) {
            break;  /* Need more data */
        }

        /* Consume the header */
        uint8_t *payload = np_recv_buf + 8;

        switch (cmd_id) {
        case RA_CMD_NETPACKET: {
            uint32_t sender_id;
            memcpy(&sender_id, payload, sizeof(sender_id));
            sender_id = ntohl(sender_id);

            const void *pkt_data = payload + 4;
            size_t pkt_len = cmd_size;  /* cmd_size = data only, client_id is extra */

            np_rx_packets++;
            np_rx_bytes += pkt_len;
            ESP_LOGD(TAG, "NETPACKET recv: sender=%u len=%u serial_mode=%d",
                     (unsigned)sender_id, (unsigned)pkt_len, serial_mode);

            np_receive_dispatch(pkt_data, pkt_len, (uint16_t)sender_id);
            break;
        }

        case RA_CMD_PING_REQUEST: {
            uint32_t pong[2];
            pong[0] = htonl(RA_CMD_PING_RESPONSE);
            pong[1] = htonl(0);
            np_queue_control_packet(pong, sizeof(pong));
            break;
        }

        case RA_CMD_PING_RESPONSE:
            /* Update latency tracking if needed */
            break;

        case RA_CMD_MODE: {
            /* Track mode changes (connected/disconnected events) */
            if (cmd_size >= 12) {
                uint32_t mode_frame, mode_val, mode_client;
                memcpy(&mode_frame, payload, 4);
                memcpy(&mode_val, payload + 4, 4);
                memcpy(&mode_client, payload + 8, 4);
                mode_val = ntohl(mode_val);
                mode_client = ntohl(mode_client);

                bool is_you = (mode_val & (1u << 31)) != 0;
                bool is_playing = (mode_val & (1u << 30)) != 0;
                uint16_t peer_id = (uint16_t)(mode_client & 0xFFFF);

                if (is_playing) {
                    if (!is_you) {
                        netplay_num_clients++;
                    }
                    ESP_LOGI(TAG, "Client %u now PLAYING%s (total peers=%u)",
                             (unsigned)peer_id,
                             is_you ? " (us)" : "",
                             (unsigned)netplay_num_clients);
                } else {
                    if (!is_you && netplay_num_clients > 0) {
                        netplay_num_clients--;
                    }
                    ESP_LOGI(TAG, "Client %u DISCONNECTED%s (total peers=%u)",
                             (unsigned)peer_id,
                             is_you ? " (us)" : "",
                             (unsigned)netplay_num_clients);
                }
            }
            break;
        }

        case RA_CMD_MODE_REFUSED:
            ESP_LOGW(TAG, "PLAY request refused by server");
            break;

        case RA_CMD_DISCONNECT:
            ESP_LOGW(TAG, "Server sent DISCONNECT");
            np_disconnect();
            return;

        default:
            ESP_LOGD(TAG, "Ignoring cmd 0x%04x size %u", cmd_id, (unsigned)cmd_size);
            break;
        }

        np_recv_consume(total_needed);
    }
}

/* ── Public API (matching libretro interface) ─────────────────────── */

void netpacket_poll_receive(void)
{
    np_client_cfg_t current_cfg;
    bool host_mode;
    bool host_busy;
    bool host_pending_io;
    int64_t min_interval_us;
    int64_t now_us;

    np_client_cfg_snapshot(&current_cfg);

    if (gpsp_netplay_ra_mode == NETPLAY_MODE_DISABLED) {
        return;
    }

    /* Background recv/flush/accept tasks self-tick and are notified by
     * producers on state changes and enqueues (see np_start_connect,
     * np_disconnect, np_queue_packet_wait, host_client_queue_packet_wait).
     * Avoid blanket xTaskNotifyGive() on the emu-core hot path — this
     * function runs ~hundreds of times per frame from rfu_update(). */

    if (!np_client_cfg_valid) {
        np_client_cfg_applied = current_cfg;
        np_client_cfg_valid = true;
    } else if (!np_client_cfg_equals(&np_client_cfg_applied, &current_cfg)) {
        if (np_mode_is_client(np_client_cfg_applied.mode) && np_state != STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "Client netplay settings changed, reconnecting");
            np_disconnect();
        }
        np_client_cfg_applied = current_cfg;
    }

    host_mode = (gpsp_netplay_ra_mode == NETPLAY_MODE_HOST ||
                 gpsp_netplay_ra_mode == NETPLAY_MODE_TUNNEL_HOST);

    if (host_mode) {
        /* Tear down leftover client connection immediately when entering host mode. */
        if (np_state != STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "Switching to host mode, disconnecting client");
            np_disconnect();
        }

        /* Busy if we already have peers, or tunnel room/session is not ready yet. */
        host_busy = (netplay_num_clients > 0);
        if (gpsp_netplay_ra_mode == NETPLAY_MODE_TUNNEL_HOST &&
            !netpacket_tunnel_host_room_ready()) {
            host_busy = true;
        }

        host_pending_io = netpacket_host_has_pending_io() ||
                          netpacket_tunnel_host_has_pending_io();

        now_us = esp_timer_get_time();
        if (host_pending_io) {
            np_last_host_poll_us = now_us;
            netpacket_tunnel_host_poll();
            netpacket_host_poll();
            return;
        }

        min_interval_us = host_busy
                              ? NP_HOST_POLL_BUSY_INTERVAL_US
                              : NP_HOST_POLL_IDLE_INTERVAL_US;

        if (now_us - np_last_host_poll_us < min_interval_us) {
            return;
        }
        np_last_host_poll_us = now_us;

        netpacket_tunnel_host_poll();
        netpacket_host_poll();
        return;
    }

    netpacket_tunnel_host_poll();

    /* Host mode management (also handles cleanup on mode change) */
    netpacket_host_poll();

    /* Tear down client connection if netplay was disabled */
    if (!gpsp_netplay_ra_enabled && np_state != STATE_DISCONNECTED) {
        ESP_LOGI(TAG, "Netplay disabled, disconnecting");
        np_disconnect();
        return;
    }

    if (np_state == STATE_DISCONNECTED) {
        if (gpsp_netplay_ra_enabled) {
            np_start_connect();
        }
        return;
    }

    /* Drain shadow buffer filled by np_recv_task on SERVICE_CORE.
     * No recv() syscall here — safe to call at rfu_update() frequency. */
    if (np_socket_fd >= 0) {
        np_recv_more();
    }

    switch (np_state) {
    case STATE_CONNECTING:
        np_check_connect();
        break;
    case STATE_WAIT_SERVER_HEADER:
        np_handle_server_header();
        break;
    case STATE_SEND_NICK:
        np_send_nick();
        break;
    case STATE_WAIT_SERVER_NICK:
        np_handle_server_nick();
        break;
    case STATE_SEND_INFO:
        np_send_info();
        break;
    case STATE_WAIT_SERVER_INFO:
        np_handle_server_info();
        break;
    case STATE_WAIT_SYNC:
        np_handle_sync();
        break;
    case STATE_SEND_PLAY:
        np_send_play();
        break;
    case STATE_CONNECTED:
        np_process_commands();

        /* Periodic ping + stats (every 5 seconds) */
        if (np_socket_fd >= 0) {
            int64_t now = esp_timer_get_time();
            if (now - np_last_ping_us > 5000000) {
                uint32_t ping[2];
                ping[0] = htonl(RA_CMD_PING_REQUEST);
                ping[1] = htonl(0);
                np_queue_control_packet(ping, sizeof(ping));
                np_last_ping_us = now;

                ESP_LOGI(TAG, "Stats: TX %u pkts/%u B, RX %u pkts/%u B, serial_mode=%d",
                         np_tx_packets, np_tx_bytes,
                         np_rx_packets, np_rx_bytes, serial_mode);
                np_tx_packets = np_tx_bytes = 0;
                np_rx_packets = np_rx_bytes = 0;
            }
        }
        break;
    default:
        break;
    }
}

void netpacket_send(uint16_t client_id, const void *buf, size_t len)
{
    if (gpsp_netplay_ra_mode == NETPLAY_MODE_HOST ||
        gpsp_netplay_ra_mode == NETPLAY_MODE_TUNNEL_HOST) {
        netpacket_host_send(client_id, buf, len);
        return;
    }

    if (np_state != STATE_CONNECTED || np_socket_fd < 0 || !buf || len == 0) {
        ESP_LOGD(TAG, "netpacket_send DROP: state=%d fd=%d buf=%p len=%u",
                 np_state, np_socket_fd, buf, (unsigned)len);
        return;
    }

    if (len > NETPACKET_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "Dropping oversized netpacket (%u bytes)", (unsigned)len);
        return;
    }

    /*
     * Wire format: cmd(u32) + payload_size(u32) + client_id(u32) + data
     *
     * As a client sending to server:
     *   client_id = recipient (0=host, N=peer, 0xFFFF=broadcast)
     * Server will relay to the target.
     *
     * Enqueue header + payload together to maintain atomicity.
     */
    uint32_t hdr[3];
    hdr[0] = htonl(RA_CMD_NETPACKET);
    hdr[1] = htonl((uint32_t)len);
    hdr[2] = htonl((uint32_t)client_id);

    np_tx_packets++;
    ESP_LOGD(TAG, "NETPACKET send: target=%u len=%u (queued)", (unsigned)client_id, (unsigned)len);

    if (!np_queue_packet_wait(hdr, sizeof(hdr), buf, len)) {
        ESP_LOGW(TAG, "Failed to queue netpacket target=%u len=%u",
                 (unsigned)client_id, (unsigned)len);
    }
}
