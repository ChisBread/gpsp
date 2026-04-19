/*
 * gpsp app support — RetroArch netplay TCP HOST for ESP32-P4
 *
 * Implements the host (server) side of the RetroArch Netplay protocol,
 * allowing other devices (RA clients or other ESP32-P4 units) to connect
 * for multiplayer gameplay.  Supports up to HOST_MAX_CLIENTS clients
 * (3 clients + host = 4-player GBA games via RFU).
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
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "c6_remote.h"
#include "common.h"
#include "gpsp_config.h"
#include "main.h"
#include "netpacket_queue.h"
#include "runtime_config.h"
#include "serial.h"

#include "mbedtls/md.h"

/* ── RetroArch Netplay protocol constants ─────────────────────────── */

#define RA_NETPLAY_MAGIC             0x52414E50u  /* "RANP" */
#define RA_NETPLAY_PROTOCOL_VERSION  7u
#define RA_NICK_LEN                  32
#define RA_PASS_HASH_LEN             64
#define RA_MAX_INPUT_DEVICES         16

#define RA_CMD_NICK                  0x0020u
#define RA_CMD_PASSWORD              0x0021u
#define RA_CMD_INFO                  0x0022u
#define RA_CMD_SYNC                  0x0023u
#define RA_CMD_PLAY                  0x0025u
#define RA_CMD_MODE                  0x0026u
#define RA_CMD_DISCONNECT            0x000Au
#define RA_CMD_NETPACKET             0x0048u
#define RA_CMD_PING_REQUEST          0x1100u
#define RA_CMD_PING_RESPONSE         0x1101u
#define RA_CMD_SETTING_ALLOW_PAUSE   0x2000u
#define RA_CMD_SETTING_INPUT_LATENCY 0x2001u

#define NETPACKET_BROADCAST          0xFFFFu
#define NETPACKET_MAX_PAYLOAD        2048u
#define NETPACKET_SEND_QUEUE_CAPACITY (16 * 1024u)

/* ── Per-client states ────────────────────────────────────────────── */

typedef enum {
    CLIENT_STATE_EMPTY = 0,
    CLIENT_STATE_WAIT_HEADER,
    CLIENT_STATE_WAIT_PASSWORD,
    CLIENT_STATE_WAIT_NICK,
    CLIENT_STATE_WAIT_INFO,
    CLIENT_STATE_WAIT_PLAY,
    CLIENT_STATE_CONNECTED,
} client_state_t;

/* ── Per-client data ──────────────────────────────────────────────── */

#define HOST_MAX_CLIENTS   3   /* host + 3 = 4-player GBA max */
#define HOST_RECV_BUF_SIZE 4096
#define HOST_SHADOW_BUF_SIZE 2048
#define HOST_PENDING_ACCEPT_MAX (HOST_MAX_CLIENTS + 1)

typedef struct {
    int            fd;
    client_state_t state;
    uint32_t       protocol;       /* negotiated version */
    uint32_t       assigned_id;    /* 1-based */
    uint32_t       password_salt;  /* non-zero if password required */
    char           nick[RA_NICK_LEN];
    size_t         recv_len;
    uint32_t       tx_packets, tx_bytes;
    uint32_t       rx_packets, rx_bytes;
    size_t         shadow_len;
    bool           shadow_closed;
    uint8_t        shadow_buf[HOST_SHADOW_BUF_SIZE];
    uint8_t        recv_buf[HOST_RECV_BUF_SIZE];
    netpacket_queue_t send_queue;
    uint8_t        *send_queue_storage;
} host_client_t;

extern void netpacket_notify_flush_task(void);

/* Forward declarations for handlers used by host_service_clients() */
static bool host_handle_client_header(host_client_t *c);
static bool host_handle_client_password(host_client_t *c);
static bool host_handle_client_nick(host_client_t *c);
static bool host_send_info(host_client_t *c);
static bool host_handle_client_info(host_client_t *c);
static bool host_handle_play(host_client_t *c);
static bool host_client_alloc_queue(host_client_t *c);
static void host_client_free_queue(host_client_t *c);
static bool host_client_queue_packet_wait(host_client_t *c,
                                          const void *part1, size_t part1_len,
                                          const void *part2, size_t part2_len);
static size_t host_client_flush(host_client_t *c);

static void ra_password_hash_hex(uint32_t salt, const char *password,
                                 char out_hex[RA_PASS_HASH_LEN + 1])
{
    uint8_t digest[32];
    char salted[8 + 128 + 1];
    size_t pw_len;

    if (!password)
        password = "";

    pw_len = strlen(password);
    if (pw_len > 128)
        pw_len = 128;

    /* RetroArch format: "%08X" (uppercase salt text) + password */
    snprintf(salted, sizeof(salted), "%08X", (unsigned)salt);
    memcpy(salted + 8, password, pw_len);
    salted[8 + pw_len] = '\0';

    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               (const unsigned char *)salted, 8 + pw_len, digest);

    for (size_t i = 0; i < sizeof(digest); i++)
        snprintf(out_hex + i * 2, 3, "%02x", (unsigned)digest[i]);
    out_hex[RA_PASS_HASH_LEN] = '\0';
}
static void host_process_commands(host_client_t *c);

/* ── Module state ─────────────────────────────────────────────────── */

extern u32 netplay_num_clients;
extern u32 netplay_client_id;

static const char *TAG = "gpsp_host";

static int host_listen_fd = -1;
static int64_t host_last_ping_us;

static SemaphoreHandle_t host_io_mutex;
static TaskHandle_t host_io_task_handle;
static int host_pending_accept_fd[HOST_PENDING_ACCEPT_MAX];
static uint8_t host_pending_accept_head;
static uint8_t host_pending_accept_tail;
static uint8_t host_pending_accept_count;

static GPSP_EXTRAM_BSS host_client_t host_clients[HOST_MAX_CLIENTS];

static void host_io_task(void *param);

void netpacket_host_notify_io_task(void)
{
    if (host_io_task_handle)
        xTaskNotifyGive(host_io_task_handle);
}

static bool host_pending_accept_push(int fd)
{
    if (host_pending_accept_count >= HOST_PENDING_ACCEPT_MAX)
        return false;

    host_pending_accept_fd[host_pending_accept_tail] = fd;
    host_pending_accept_tail = (uint8_t)((host_pending_accept_tail + 1) % HOST_PENDING_ACCEPT_MAX);
    host_pending_accept_count++;
    return true;
}

static int host_pending_accept_pop(void)
{
    int fd;
    if (host_pending_accept_count == 0)
        return -1;

    fd = host_pending_accept_fd[host_pending_accept_head];
    host_pending_accept_head = (uint8_t)((host_pending_accept_head + 1) % HOST_PENDING_ACCEPT_MAX);
    host_pending_accept_count--;
    return fd;
}

/* ── Helpers ──────────────────────────────────────────────────────── */

static uint32_t host_platform_magic(void)
{
    return (0u << 30) | (sizeof(size_t) << 15) | sizeof(long);
}

static uint32_t host_impl_magic(void)
{
    const char *ver = GPSP_VERSION;
    uint32_t magic = 0;
    size_t i;

    for (i = 0; ver[i]; i++)
        magic ^= (uint32_t)ver[i] << (i & 0xf);
    magic ^= RA_NETPLAY_PROTOCOL_VERSION << (i & 0xf);

    return magic;
}

static int host_count_connected(void)
{
    int n = 0;
    for (int i = 0; i < HOST_MAX_CLIENTS; i++)
        if (host_clients[i].state == CLIENT_STATE_CONNECTED)
            n++;
    return n;
}

static host_client_t *host_find_by_id(uint32_t assigned_id)
{
    for (int i = 0; i < HOST_MAX_CLIENTS; i++)
        if (host_clients[i].state != CLIENT_STATE_EMPTY &&
            host_clients[i].assigned_id == assigned_id)
            return &host_clients[i];
    return NULL;
}

static uint32_t host_next_client_id(void)
{
    for (uint32_t id = 1; id <= HOST_MAX_CLIENTS; id++)
        if (!host_find_by_id(id))
            return id;
    return 0; /* full */
}

static void host_update_num_clients(void)
{
    netplay_num_clients = (u32)host_count_connected();
}

static bool host_is_active(void)
{
    if (host_listen_fd >= 0) return true;
    for (int i = 0; i < HOST_MAX_CLIENTS; i++)
        if (host_clients[i].state != CLIENT_STATE_EMPTY)
            return true;
    return false;
}

static host_client_t *host_alloc_client_slot(uint32_t *assigned_id)
{
    host_client_t *c = NULL;
    uint32_t id = host_next_client_id();

    if (id == 0)
        return NULL;

    for (int i = 0; i < HOST_MAX_CLIENTS; i++) {
        if (host_clients[i].state == CLIENT_STATE_EMPTY) {
            c = &host_clients[i];
            break;
        }
    }

    if (!c)
        return NULL;

    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->assigned_id = id;
    netpacket_queue_init(&c->send_queue, NULL, 0);

    if (!host_client_alloc_queue(c))
        return NULL;

    if (assigned_id)
        *assigned_id = id;

    return c;
}

static bool host_prepare_client_socket(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return true;
}

/* ── Per-client TCP helpers ───────────────────────────────────────── */

static bool host_client_alloc_queue(host_client_t *c)
{
    if (!c)
        return false;

    if (c->send_queue_storage)
        return true;

    c->send_queue_storage = heap_caps_malloc(NETPACKET_SEND_QUEUE_CAPACITY,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c->send_queue_storage) {
        ESP_LOGE(TAG, "Client %u: failed to allocate send queue (%u bytes)",
                 c->assigned_id, (unsigned)NETPACKET_SEND_QUEUE_CAPACITY);
        return false;
    }

    netpacket_queue_init(&c->send_queue, c->send_queue_storage,
                         NETPACKET_SEND_QUEUE_CAPACITY);
    return true;
}

static void host_client_free_queue(host_client_t *c)
{
    if (!c || !c->send_queue_storage)
        return;

    heap_caps_free(c->send_queue_storage);
    c->send_queue_storage = NULL;
    netpacket_queue_init(&c->send_queue, NULL, 0);
}

static bool host_client_queue_packet_wait(host_client_t *c,
                                          const void *part1, size_t part1_len,
                                          const void *part2, size_t part2_len)
{
    if (!host_client_alloc_queue(c))
        return false;

    while (c->fd >= 0 && c->state == CLIENT_STATE_CONNECTED) {
        if (netpacket_queue_enqueue2(&c->send_queue, part1, part1_len,
                                     part2, part2_len)) {
            netpacket_notify_flush_task();
            return true;
        }

        netpacket_notify_flush_task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return false;
}

static size_t host_client_flush(host_client_t *c)
{
    uint8_t *data;
    size_t len;
    size_t total_flushed = 0;

    if (!c || c->fd < 0 || c->state != CLIENT_STATE_CONNECTED)
        return 0;

    while ((len = netpacket_queue_peek_contiguous(&c->send_queue, &data)) > 0) {
        ssize_t n = send(c->fd, data, len, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            ESP_LOGW(TAG, "Client %u: send failed errno=%d",
                     c->assigned_id, errno);
            break;
        }

        if (n == 0)
            break;

        netpacket_queue_consume(&c->send_queue, (size_t)n);
        total_flushed += (size_t)n;
    }

    return total_flushed;
}

static bool client_send_all(host_client_t *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(c->fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            ESP_LOGW(TAG, "Client %u: send failed errno=%d",
                     c->assigned_id, errno);
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static ssize_t client_recv_more(host_client_t *c)
{
    if (host_io_mutex && xSemaphoreTake(host_io_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
        if (c->shadow_closed) {
            c->shadow_closed = false;
            xSemaphoreGive(host_io_mutex);
            return -1;
        }

        if (c->shadow_len > 0 && c->recv_len < HOST_RECV_BUF_SIZE) {
            size_t space = HOST_RECV_BUF_SIZE - c->recv_len;
            size_t copy = c->shadow_len < space ? c->shadow_len : space;
            memcpy(c->recv_buf + c->recv_len, c->shadow_buf, copy);
            c->recv_len += copy;
            c->shadow_len -= copy;
            if (c->shadow_len > 0)
                memmove(c->shadow_buf, c->shadow_buf + copy, c->shadow_len);
        }
        xSemaphoreGive(host_io_mutex);
    }

    if (c->recv_len >= HOST_RECV_BUF_SIZE)
        return (ssize_t)c->recv_len;

    if (host_io_task_handle)
        return (ssize_t)c->recv_len;

    ssize_t n = recv(c->fd, c->recv_buf + c->recv_len,
                     HOST_RECV_BUF_SIZE - c->recv_len, MSG_DONTWAIT);
    if (n > 0) {
        c->recv_len += (size_t)n;
    } else if (n == 0) {
        return -1;  /* peer closed */
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        return -1;
    }
    return (ssize_t)c->recv_len;
}

static void client_recv_consume(host_client_t *c, size_t n)
{
    if (n >= c->recv_len)
        c->recv_len = 0;
    else {
        memmove(c->recv_buf, c->recv_buf + n, c->recv_len - n);
        c->recv_len -= n;
    }
}

static bool client_try_read(host_client_t *c, void *out, size_t needed)
{
    if (c->recv_len < needed) {
        if (client_recv_more(c) < 0) return false;
    }
    if (c->recv_len < needed) return false;
    memcpy(out, c->recv_buf, needed);
    client_recv_consume(c, needed);
    return true;
}

/* ── Disconnect / cleanup ─────────────────────────────────────────── */

static void host_disconnect_client(host_client_t *c)
{
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }

    ESP_LOGI(TAG, "Client %u (\"%s\") disconnected",
             c->assigned_id, c->nick);

    bool was_connected = (c->state == CLIENT_STATE_CONNECTED);
    c->state    = CLIENT_STATE_EMPTY;
    c->recv_len = 0;
    c->shadow_len = 0;
    c->shadow_closed = false;
    host_client_free_queue(c);

    if (was_connected) {
        host_update_num_clients();

        /* Reset serial protocols when last client leaves */
        if (netplay_num_clients == 0) {
            serialproto_reset();
            rfu_reset();
            serial_reset_irq();
        }
    }
}

static void host_stop(void)
{
    for (int i = 0; i < HOST_MAX_CLIENTS; i++) {
        if (host_clients[i].fd >= 0) {
            close(host_clients[i].fd);
            host_clients[i].fd = -1;
        }
        host_clients[i].state    = CLIENT_STATE_EMPTY;
        host_clients[i].recv_len = 0;
        host_clients[i].shadow_len = 0;
        host_clients[i].shadow_closed = false;
        host_client_free_queue(&host_clients[i]);
    }
    if (host_listen_fd >= 0) {
        close(host_listen_fd);
        host_listen_fd = -1;
    }
    netplay_num_clients = 0;
    netplay_client_id   = 0;
    if (host_io_mutex && xSemaphoreTake(host_io_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        host_pending_accept_count = 0;
        host_pending_accept_head = 0;
        host_pending_accept_tail = 0;
        xSemaphoreGive(host_io_mutex);
    }

    serialproto_reset();
    rfu_reset();
    serial_reset_irq();
}

/* ── Receive dispatch ─────────────────────────────────────────────── */

static void host_receive_dispatch(const void *buf, size_t len,
                                  uint16_t client_id)
{
    switch (serial_mode) {
    case SERIAL_MODE_RFU:
        rfu_net_receive(buf, len, client_id);
        break;
    case SERIAL_MODE_SERIAL_POKE:
        serialpoke_net_receive(buf, len, client_id);
        break;
    case SERIAL_MODE_SERIAL_AW1:
    case SERIAL_MODE_SERIAL_AW2:
        serialaw_net_receive(buf, len, client_id);
        break;
    default:
        break;
    }
}

/* ── Listener ─────────────────────────────────────────────────────── */

static bool host_start_listen(void)
{
    if (host_listen_fd >= 0) return true;
    if (!c6_remote_network_ready()) return false;

    int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        ESP_LOGW(TAG, "Failed to create listen socket: errno=%d", errno);
        return false;
    }

    int one = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags >= 0) fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(gpsp_netplay_ra_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sockfd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "Bind failed on port %u: errno=%d",
                 gpsp_netplay_ra_port, errno);
        close(sockfd);
        return false;
    }

    if (listen(sockfd, HOST_MAX_CLIENTS) < 0) {
        ESP_LOGW(TAG, "Listen failed: errno=%d", errno);
        close(sockfd);
        return false;
    }

    host_listen_fd    = sockfd;
    netplay_client_id = 0;  /* Host is always client 0 */
    ESP_LOGI(TAG, "Listening on port %u (max %d clients)",
             gpsp_netplay_ra_port, HOST_MAX_CLIENTS);
    return true;
}

static void host_check_accept(void)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int fd = accept(host_listen_fd,
                    (struct sockaddr *)&client_addr, &addr_len);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        ESP_LOGW(TAG, "Accept failed: errno=%d", errno);
        return;
    }

    /* Find a free slot */
    uint32_t id = 0;
    host_client_t *c = host_alloc_client_slot(&id);
    if (!c) {
        ESP_LOGW(TAG, "No free client slots, rejecting connection");
        close(fd);
        return;
    }

    /* Non-blocking + TCP_NODELAY */
    host_prepare_client_socket(fd);
    c->fd          = fd;
    c->state       = CLIENT_STATE_WAIT_HEADER;
    c->assigned_id = id;

    char addr_str[INET_ADDRSTRLEN];
    inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str));
    ESP_LOGI(TAG, "Client %u connected from %s:%u",
             id, addr_str, ntohs(client_addr.sin_port));
}

static void host_drain_pending_accept(void)
{
    for (;;) {
        int fd = -1;
        uint32_t id = 0;
        host_client_t *c;

        if (host_io_mutex && xSemaphoreTake(host_io_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
            fd = host_pending_accept_pop();
            xSemaphoreGive(host_io_mutex);
        }

        if (fd < 0)
            break;

        c = host_alloc_client_slot(&id);
        if (!c) {
            ESP_LOGW(TAG, "No free client slots, rejecting connection");
            close(fd);
            continue;
        }

        host_prepare_client_socket(fd);
        c->fd          = fd;
        c->state       = CLIENT_STATE_WAIT_HEADER;
        c->assigned_id = id;

        ESP_LOGI(TAG, "Client %u connected", id);
    }
}

bool netpacket_host_attach_client(int fd, const char *peer_desc)
{
    uint32_t id = 0;
    host_client_t *c;

    if (fd < 0)
        return false;

    c = host_alloc_client_slot(&id);
    if (!c) {
        ESP_LOGW(TAG, "No free host slots for tunnel client %s",
                 peer_desc ? peer_desc : "<unknown>");
        close(fd);
        return false;
    }

    host_prepare_client_socket(fd);
    c->fd = fd;
    c->state = CLIENT_STATE_WAIT_HEADER;
    c->assigned_id = id;

    ESP_LOGI(TAG, "Attached tunnel client %u from %s",
             (unsigned)id, peer_desc ? peer_desc : "<unknown>");
    return true;
}

static void host_service_clients(void)
{
    /* Service each client */
    for (int i = 0; i < HOST_MAX_CLIENTS; i++) {
        host_client_t *c = &host_clients[i];
        if (c->state == CLIENT_STATE_EMPTY) continue;

        /* Read data from client */
        if (client_recv_more(c) < 0) {
            ESP_LOGW(TAG, "Client %u: connection lost", c->assigned_id);
            host_disconnect_client(c);
            continue;
        }

        switch (c->state) {
        case CLIENT_STATE_WAIT_HEADER:
            host_handle_client_header(c);
            break;
        case CLIENT_STATE_WAIT_PASSWORD:
            host_handle_client_password(c);
            break;
        case CLIENT_STATE_WAIT_NICK:
            host_handle_client_nick(c);
            break;
        case CLIENT_STATE_WAIT_INFO:
            host_handle_client_info(c);
            break;
        case CLIENT_STATE_WAIT_PLAY:
            host_handle_play(c);
            break;
        case CLIENT_STATE_CONNECTED:
            host_process_commands(c);
            break;
        default:
            break;
        }

    }

    /* Periodic ping + stats (every 5 seconds) */
    int64_t now = esp_timer_get_time();
    if (now - host_last_ping_us > 5000000) {
        host_last_ping_us = now;

        uint32_t ping[2];
        ping[0] = htonl(RA_CMD_PING_REQUEST);
        ping[1] = htonl(0);

        for (int i = 0; i < HOST_MAX_CLIENTS; i++) {
            host_client_t *c = &host_clients[i];
            if (c->state != CLIENT_STATE_CONNECTED) continue;

            host_client_queue_packet_wait(c, ping, sizeof(ping), NULL, 0);

            if (c->tx_packets || c->rx_packets) {
                ESP_LOGI(TAG, "Client %u: TX %u pkts/%u B, RX %u pkts/%u B",
                         c->assigned_id,
                         c->tx_packets, c->tx_bytes,
                         c->rx_packets, c->rx_bytes);
                c->tx_packets = c->tx_bytes = 0;
                c->rx_packets = c->rx_bytes = 0;
            }
        }
    }
}

/* ── Per-client handshake phases ──────────────────────────────────── */

static bool host_handle_client_header(host_client_t *c)
{
    uint32_t header[6];
    if (!client_try_read(c, header, sizeof(header))) return false;

    if (ntohl(header[0]) != RA_NETPLAY_MAGIC) {
        ESP_LOGE(TAG, "Client %u: bad magic 0x%08x",
                 c->assigned_id, ntohl(header[0]));
        host_disconnect_client(c);
        return false;
    }

    uint32_t client_proto = ntohl(header[3]);
    ESP_LOGI(TAG, "Client %u: protocol %u, platform 0x%08x",
             c->assigned_id, (unsigned)client_proto, ntohl(header[1]));

    c->protocol = client_proto < RA_NETPLAY_PROTOCOL_VERSION
                      ? client_proto : RA_NETPLAY_PROTOCOL_VERSION;
    if (c->protocol < 5) {
        ESP_LOGE(TAG, "Client %u: protocol too old (%u)",
                 c->assigned_id, (unsigned)client_proto);
        host_disconnect_client(c);
        return false;
    }

    uint32_t response[6];
    response[0] = htonl(RA_NETPLAY_MAGIC);
    response[1] = htonl(host_platform_magic());
    response[2] = htonl(0);                    /* No compression */
    response[4] = htonl(c->protocol);          /* Negotiated version */
    response[5] = htonl(host_impl_magic());

    /* Password: generate random salt if host password is configured */
    if (gpsp_netplay_host_password[0] != '\0') {
        uint32_t salt = esp_random();
        if (salt == 0) salt = 1;  /* ensure non-zero */
        c->password_salt = salt;
        response[3] = htonl(salt);
    } else {
        c->password_salt = 0;
        response[3] = htonl(0);
    }

    if (!client_send_all(c, response, sizeof(response))) {
        host_disconnect_client(c);
        return false;
    }

    /* RetroArch handshake order is: HEADER -> NICK -> (optional PASSWORD) -> INFO */
    c->state = CLIENT_STATE_WAIT_NICK;
    return true;
}

static bool host_handle_client_password(host_client_t *c)
{
    uint32_t cmd[2];
    char recv_hash[RA_PASS_HASH_LEN];

    if (!client_try_read(c, cmd, sizeof(cmd))) return false;

    if (ntohl(cmd[0]) != RA_CMD_PASSWORD || ntohl(cmd[1]) != RA_PASS_HASH_LEN) {
        ESP_LOGE(TAG, "Client %u: expected PASSWORD, got 0x%04x size=%u",
                 c->assigned_id, ntohl(cmd[0]), (unsigned)ntohl(cmd[1]));
        host_disconnect_client(c);
        return false;
    }

    if (!client_try_read(c, recv_hash, sizeof(recv_hash))) return false;

    /* RetroArch format: SHA256("%08X" + password) as 64-byte lowercase hex */
    char expected[RA_PASS_HASH_LEN + 1];
    ra_password_hash_hex(c->password_salt, gpsp_netplay_host_password, expected);

    if (memcmp(recv_hash, expected, RA_PASS_HASH_LEN) != 0) {
        ESP_LOGE(TAG, "Client %u: password mismatch", c->assigned_id);
        host_disconnect_client(c);
        return false;
    }

    ESP_LOGI(TAG, "Client %u: password OK", c->assigned_id);
    return host_send_info(c);
}

static bool host_handle_client_nick(host_client_t *c)
{
    uint32_t cmd[2];
    if (!client_try_read(c, cmd, sizeof(cmd))) return false;

    if (ntohl(cmd[0]) != RA_CMD_NICK || ntohl(cmd[1]) != RA_NICK_LEN) {
        ESP_LOGE(TAG, "Client %u: expected NICK, got 0x%04x",
                 c->assigned_id, ntohl(cmd[0]));
        host_disconnect_client(c);
        return false;
    }

    if (!client_try_read(c, c->nick, RA_NICK_LEN)) {
        host_disconnect_client(c);
        return false;
    }
    c->nick[RA_NICK_LEN - 1] = '\0';
    ESP_LOGI(TAG, "Client %u nick: \"%s\"", c->assigned_id, c->nick);

    /* Send our NICK */
    uint32_t nick_cmd[2];
    char our_nick[RA_NICK_LEN];
    memset(our_nick, 0, sizeof(our_nick));
    strlcpy(our_nick, gpsp_netplay_ra_nick, sizeof(our_nick));

    nick_cmd[0] = htonl(RA_CMD_NICK);
    nick_cmd[1] = htonl(RA_NICK_LEN);

    if (!client_send_all(c, nick_cmd, sizeof(nick_cmd)) ||
        !client_send_all(c, our_nick, sizeof(our_nick))) {
        host_disconnect_client(c);
        return false;
    }

    if (c->password_salt != 0) {
        c->state = CLIENT_STATE_WAIT_PASSWORD;
        return true;
    }

    return host_send_info(c);
}

static bool host_send_info(host_client_t *c)
{
    /* Send INFO after password phase (or immediately if no password required). */
    struct {
        uint32_t cmd[2];
        uint32_t content_crc;
        char core_name[32];
        char core_version[32];
    } __attribute__((packed)) info;
    memset(&info, 0, sizeof(info));

    info.cmd[0] = htonl(RA_CMD_INFO);
    info.cmd[1] = htonl(sizeof(info) - sizeof(info.cmd));
    info.content_crc = htonl(0);
    strlcpy(info.core_name, GPSP_NAME, sizeof(info.core_name));
    strlcpy(info.core_version, GPSP_NETPACKET_VERSION,
            sizeof(info.core_version));

    if (!client_send_all(c, &info, sizeof(info))) {
        host_disconnect_client(c);
        return false;
    }

    c->state = CLIENT_STATE_WAIT_INFO;
    return true;
}

static bool host_handle_client_info(host_client_t *c)
{
    uint32_t cmd[2];
    if (!client_try_read(c, cmd, sizeof(cmd))) return false;

    uint32_t cmd_id   = ntohl(cmd[0]);
    uint32_t cmd_size = ntohl(cmd[1]);

    if (cmd_id != RA_CMD_INFO) {
        ESP_LOGE(TAG, "Client %u: expected INFO, got 0x%04x",
                 c->assigned_id, cmd_id);
        host_disconnect_client(c);
        return false;
    }

    /* Consume client INFO payload */
    while (cmd_size > 0) {
        uint8_t discard[256];
        size_t chunk = cmd_size < sizeof(discard) ? cmd_size : sizeof(discard);
        if (!client_try_read(c, discard, chunk)) {
            host_disconnect_client(c);
            return false;
        }
        cmd_size -= chunk;
    }

    /* INFO was already sent in the NICK phase.  Now send SYNC. */
    struct __attribute__((packed)) {
        uint32_t cmd[2];
        uint32_t frame_count;
        uint32_t client_num;
        uint32_t devices[RA_MAX_INPUT_DEVICES];
        uint8_t  share_modes[RA_MAX_INPUT_DEVICES];
        uint32_t device_clients[RA_MAX_INPUT_DEVICES];
        char     nick[RA_NICK_LEN];
    } sync;
    memset(&sync, 0, sizeof(sync));

    sync.cmd[0]       = htonl(RA_CMD_SYNC);
    sync.cmd[1]       = htonl(sizeof(sync) - sizeof(sync.cmd));
    sync.frame_count   = htonl(0);
    sync.client_num    = htonl(c->assigned_id);
    sync.device_clients[0] = htonl(0);  /* slot 0 → host */
    if (c->assigned_id <= RA_MAX_INPUT_DEVICES)
        sync.device_clients[c->assigned_id] = htonl(c->assigned_id);
    /* Echo client's own nick (RA uses this to detect server-forced renames) */
    memcpy(sync.nick, c->nick, sizeof(sync.nick));

    if (!client_send_all(c, &sync, sizeof(sync))) {
        host_disconnect_client(c);
        return false;
    }

    /* Send SETTING commands (protocol v6+) */
    if (c->protocol >= 6) {
        uint32_t setting_cmd[3];
        setting_cmd[0] = htonl(RA_CMD_SETTING_ALLOW_PAUSE);
        setting_cmd[1] = htonl(4);
        setting_cmd[2] = htonl(1);
        if (!client_send_all(c, setting_cmd, sizeof(setting_cmd))) {
            host_disconnect_client(c);
            return false;
        }

        uint32_t latency_cmd[4];
        latency_cmd[0] = htonl(RA_CMD_SETTING_INPUT_LATENCY);
        latency_cmd[1] = htonl(8);
        latency_cmd[2] = htonl(0);
        latency_cmd[3] = htonl(0);
        if (!client_send_all(c, latency_cmd, sizeof(latency_cmd))) {
            host_disconnect_client(c);
            return false;
        }
    }

    ESP_LOGI(TAG, "Client %u: sent SYNC (assigned_id=%u)",
             c->assigned_id, c->assigned_id);

    c->state = CLIENT_STATE_WAIT_PLAY;
    return true;
}

static bool host_handle_play(host_client_t *c)
{
    uint32_t cmd[2];
    if (!client_try_read(c, cmd, sizeof(cmd))) return false;

    uint32_t cmd_id   = ntohl(cmd[0]);
    uint32_t cmd_size = ntohl(cmd[1]);

    if (cmd_id != RA_CMD_PLAY) {
        /* Skip pre-PLAY commands (SETTING etc.) */
        ESP_LOGD(TAG, "Client %u: pre-PLAY cmd 0x%04x, skipping",
                 c->assigned_id, cmd_id);
        while (cmd_size > 0) {
            uint8_t discard[256];
            size_t chunk = cmd_size < sizeof(discard) ? cmd_size : sizeof(discard);
            if (!client_try_read(c, discard, chunk)) {
                host_disconnect_client(c);
                return false;
            }
            cmd_size -= chunk;
        }
        return false;  /* try again next poll */
    }

    /* Consume PLAY payload */
    while (cmd_size > 0) {
        uint8_t discard[64];
        size_t chunk = cmd_size < sizeof(discard) ? cmd_size : sizeof(discard);
        if (!client_try_read(c, discard, chunk)) {
            host_disconnect_client(c);
            return false;
        }
        cmd_size -= chunk;
    }

    /* Send MODE to confirm: "you are now playing"
     * RA expects: frame(4) + mode(4) + devices(4) + share_modes(16) + nick(32) = 60 bytes */
    struct __attribute__((packed)) {
        uint32_t cmd[2];
        uint32_t frame;
        uint32_t mode;          /* YOU(31) | PLAYING(30) | client_num(low bits) */
        uint32_t devices;
        uint8_t  share_modes[RA_MAX_INPUT_DEVICES];
        char     nick[RA_NICK_LEN];
    } mode_msg;
    memset(&mode_msg, 0, sizeof(mode_msg));

    mode_msg.cmd[0]  = htonl(RA_CMD_MODE);
    mode_msg.cmd[1]  = htonl(sizeof(mode_msg) - sizeof(mode_msg.cmd));  /* 60 */
    mode_msg.frame   = htonl(0);
    mode_msg.mode    = htonl((1u << 31) | (1u << 30) | c->assigned_id);
    mode_msg.devices = htonl(0);
    memcpy(mode_msg.nick, c->nick, sizeof(mode_msg.nick));

    if (!client_send_all(c, &mode_msg, sizeof(mode_msg))) {
        host_disconnect_client(c);
        return false;
    }

    c->state          = CLIENT_STATE_CONNECTED;
    netplay_client_id = 0;  /* host is always 0 */
    host_update_num_clients();

    ESP_LOGI(TAG, "Client %u (\"%s\") now PLAYING — %u client(s) active",
             c->assigned_id, c->nick, netplay_num_clients);
    return true;
}

/* ── Runtime command processing (per-client) ──────────────────────── */

static void host_process_commands(host_client_t *c)
{
    while (c->recv_len >= 8 && c->state == CLIENT_STATE_CONNECTED) {
        uint32_t cmd_hdr[2];
        memcpy(cmd_hdr, c->recv_buf, sizeof(cmd_hdr));
        uint32_t cmd_id   = ntohl(cmd_hdr[0]);
        uint32_t cmd_size = ntohl(cmd_hdr[1]);

        size_t total_needed;
        if (cmd_id == RA_CMD_NETPACKET)
            total_needed = 8 + 4 + cmd_size;   /* hdr + client_id + data */
        else
            total_needed = 8 + cmd_size;

        if (c->recv_len < total_needed) break;

        uint8_t *payload = c->recv_buf + 8;

        switch (cmd_id) {
        case RA_CMD_NETPACKET: {
            /* The client_id field from client→host is the DESTINATION,
             * but the serial protocols need the SENDER id.  Use the
             * client's actual assigned_id as the sender. */
            const void *pkt_data = payload + 4;
            size_t pkt_len = cmd_size;

            c->rx_packets++;
            c->rx_bytes += pkt_len;

            host_receive_dispatch(pkt_data, pkt_len,
                                  (uint16_t)c->assigned_id);
            break;
        }

        case RA_CMD_PING_REQUEST: {
            uint32_t pong[2];
            pong[0] = htonl(RA_CMD_PING_RESPONSE);
            pong[1] = htonl(0);
            host_client_queue_packet_wait(c, pong, sizeof(pong), NULL, 0);
            break;
        }

        case RA_CMD_PING_RESPONSE:
            break;

        case RA_CMD_DISCONNECT:
            ESP_LOGW(TAG, "Client %u sent DISCONNECT", c->assigned_id);
            host_disconnect_client(c);
            return;

        default:
            ESP_LOGD(TAG, "Client %u: ignoring cmd 0x%04x",
                     c->assigned_id, cmd_id);
            break;
        }

        client_recv_consume(c, total_needed);
    }
}

/* ── Public API (called from netpacket_bridge_ra.c) ───────────────── */

void netpacket_host_poll(void)
{
    /* If mode is not host-compatible, clean up any leftover state and return */
    if (gpsp_netplay_ra_mode != NETPLAY_MODE_HOST &&
        gpsp_netplay_ra_mode != NETPLAY_MODE_TUNNEL_HOST) {
        if (host_is_active())
            host_stop();
        return;
    }

    if (gpsp_netplay_ra_mode == NETPLAY_MODE_HOST) {
        /* Start listener if needed */
        if (host_listen_fd < 0) {
            host_start_listen();
            return;
        }

        if (!host_io_task_handle) {
            /* Accept new connections (fallback path without background IO) */
            host_check_accept();
        } else {
            host_drain_pending_accept();
        }
    } else if (host_listen_fd >= 0) {
        close(host_listen_fd);
        host_listen_fd = -1;
    }

    host_service_clients();
}

esp_err_t netpacket_host_background_start(BaseType_t core_id, UBaseType_t priority)
{
    BaseType_t ret;

    if (host_io_task_handle)
        return ESP_OK;

    if (!host_io_mutex) {
        host_io_mutex = xSemaphoreCreateMutex();
        if (!host_io_mutex)
            return ESP_FAIL;
    }

    ret = xTaskCreatePinnedToCore(host_io_task, "np_host_io", 2048, NULL,
                                  priority, &host_io_task_handle, core_id);
    if (ret != pdPASS) {
        host_io_task_handle = NULL;
        ESP_LOGW(TAG, "Failed to create host IO task; fallback to sync host poll");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void host_io_task(void *param)
{
    (void)param;
    uint8_t tmp[256];

    for (;;) {
        bool did_work = false;

        if (gpsp_netplay_ra_mode != NETPLAY_MODE_HOST &&
            gpsp_netplay_ra_mode != NETPLAY_MODE_TUNNEL_HOST) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (gpsp_netplay_ra_mode == NETPLAY_MODE_HOST && host_listen_fd >= 0) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int fd = accept(host_listen_fd, (struct sockaddr *)&client_addr, &addr_len);
            if (fd >= 0) {
                if (host_io_mutex && xSemaphoreTake(host_io_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
                    if (!host_pending_accept_push(fd)) {
                        close(fd);
                    } else {
                        did_work = true;
                    }
                    xSemaphoreGive(host_io_mutex);
                } else {
                    close(fd);
                }
            }
        }

        for (int i = 0; i < HOST_MAX_CLIENTS; i++) {
            host_client_t *c = &host_clients[i];
            size_t space = 0;
            size_t want = 0;
            if (c->state == CLIENT_STATE_EMPTY || c->fd < 0)
                continue;

            if (host_io_mutex && xSemaphoreTake(host_io_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
                space = HOST_SHADOW_BUF_SIZE - c->shadow_len;
                xSemaphoreGive(host_io_mutex);
            }

            if (space == 0)
                continue;

            want = space < sizeof(tmp) ? space : sizeof(tmp);
            ssize_t n = recv(c->fd, tmp, want, MSG_DONTWAIT);
            if (n > 0) {
                if (host_io_mutex && xSemaphoreTake(host_io_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
                    size_t copy = (size_t)n;
                    memcpy(c->shadow_buf + c->shadow_len, tmp, copy);
                    c->shadow_len += copy;
                    did_work = true;
                    xSemaphoreGive(host_io_mutex);
                }
            } else if (n == 0) {
                if (host_io_mutex && xSemaphoreTake(host_io_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
                    c->shadow_closed = true;
                    xSemaphoreGive(host_io_mutex);
                }
            }
        }

        if (!did_work)
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        else
            taskYIELD();
    }
}

bool netpacket_host_has_pending_io(void)
{
    bool pending = false;

    /* Without async IO task, keep previous synchronous behavior. */
    if (!host_io_task_handle)
        return true;

    if (!host_io_mutex)
        return false;

    if (xSemaphoreTake(host_io_mutex, 0) != pdTRUE)
        return true;

    if (host_pending_accept_count > 0) {
        pending = true;
    } else {
        for (int i = 0; i < HOST_MAX_CLIENTS; i++) {
            host_client_t *c = &host_clients[i];
            if (c->state == CLIENT_STATE_EMPTY)
                continue;
            if (c->shadow_len > 0 || c->shadow_closed) {
                pending = true;
                break;
            }
        }
    }

    xSemaphoreGive(host_io_mutex);
    return pending;
}

size_t netpacket_host_flush_queued(void)
{
    size_t total_flushed = 0;

    for (int i = 0; i < HOST_MAX_CLIENTS; i++) {
        host_client_t *c = &host_clients[i];
        if (c->state != CLIENT_STATE_CONNECTED)
            continue;
        total_flushed += host_client_flush(c);
    }

    return total_flushed;
}

void netpacket_host_send(uint16_t client_id, const void *buf, size_t len)
{
    if (!buf || len == 0) return;

    if (len > NETPACKET_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "Dropping oversized netpacket (%u bytes)",
                 (unsigned)len);
        return;
    }

    /* Header: cmd + payload_size + sender_id.
     * Per RA protocol, sender_id = 0 when data originates from host. */
    uint32_t hdr[3];
    hdr[0] = htonl(RA_CMD_NETPACKET);
    hdr[1] = htonl((uint32_t)len);
    hdr[2] = htonl(0);  /* sender = host */

    if (client_id == NETPACKET_BROADCAST) {
        for (int i = 0; i < HOST_MAX_CLIENTS; i++) {
            host_client_t *c = &host_clients[i];
            if (c->state != CLIENT_STATE_CONNECTED) continue;

            c->tx_packets++;
            if (!host_client_queue_packet_wait(c, hdr, sizeof(hdr), buf, len))
                ESP_LOGW(TAG, "Failed to queue packet for client %u", c->assigned_id);
        }
    } else {
        host_client_t *c = host_find_by_id(client_id);
        if (!c || c->state != CLIENT_STATE_CONNECTED) return;

        c->tx_packets++;
        if (!host_client_queue_packet_wait(c, hdr, sizeof(hdr), buf, len))
            ESP_LOGW(TAG, "Failed to queue packet for client %u", c->assigned_id);
    }
}
