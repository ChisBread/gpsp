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
#include <string.h>
#include <unistd.h>

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "c6_remote.h"
#include "common.h"
#include "gpsp_config.h"
#include "main.h"
#include "runtime_config.h"
#include "serial.h"

/* ── RetroArch Netplay protocol constants ─────────────────────────── */

#define RA_NETPLAY_MAGIC             0x52414E50u  /* "RANP" */
#define RA_NETPLAY_PROTOCOL_VERSION  7u
#define RA_NICK_LEN                  32
#define RA_MAX_INPUT_DEVICES         16

#define RA_CMD_NICK                  0x0020u
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

/* ── Per-client states ────────────────────────────────────────────── */

typedef enum {
    CLIENT_STATE_EMPTY = 0,
    CLIENT_STATE_WAIT_HEADER,
    CLIENT_STATE_WAIT_NICK,
    CLIENT_STATE_WAIT_INFO,
    CLIENT_STATE_WAIT_PLAY,
    CLIENT_STATE_CONNECTED,
} client_state_t;

/* ── Per-client data ──────────────────────────────────────────────── */

#define HOST_MAX_CLIENTS   3   /* host + 3 = 4-player GBA max */
#define HOST_RECV_BUF_SIZE 4096

typedef struct {
    int            fd;
    client_state_t state;
    uint32_t       protocol;       /* negotiated version */
    uint32_t       assigned_id;    /* 1-based */
    char           nick[RA_NICK_LEN];
    size_t         recv_len;
    uint32_t       tx_packets, tx_bytes;
    uint32_t       rx_packets, rx_bytes;
    uint8_t        recv_buf[HOST_RECV_BUF_SIZE];
} host_client_t;

/* Forward declarations for handlers used by host_service_clients() */
static bool host_handle_client_header(host_client_t *c);
static bool host_handle_client_nick(host_client_t *c);
static bool host_handle_client_info(host_client_t *c);
static bool host_handle_play(host_client_t *c);
static void host_process_commands(host_client_t *c);

/* ── Module state ─────────────────────────────────────────────────── */

extern u32 netplay_num_clients;
extern u32 netplay_client_id;

static const char *TAG = "gpsp_host";

static int host_listen_fd = -1;
static int64_t host_last_ping_us;

static GPSP_EXTRAM_BSS host_client_t host_clients[HOST_MAX_CLIENTS];

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
    if (c->recv_len >= HOST_RECV_BUF_SIZE)
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
    }
    if (host_listen_fd >= 0) {
        close(host_listen_fd);
        host_listen_fd = -1;
    }
    netplay_num_clients = 0;
    netplay_client_id   = 0;

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

            client_send_all(c, ping, sizeof(ping));

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
    response[3] = htonl(0);                    /* No password */
    response[4] = htonl(c->protocol);          /* Negotiated version */
    response[5] = htonl(host_impl_magic());

    if (!client_send_all(c, response, sizeof(response))) {
        host_disconnect_client(c);
        return false;
    }

    c->state = CLIENT_STATE_WAIT_NICK;
    return true;
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

    /* Send INFO immediately after NICK (RA client expects this order) */
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
            client_send_all(c, pong, sizeof(pong));
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

        /* Accept new connections */
        host_check_accept();
    } else if (host_listen_fd >= 0) {
        close(host_listen_fd);
        host_listen_fd = -1;
    }

    host_service_clients();
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
        /* Send to all connected clients */
        for (int i = 0; i < HOST_MAX_CLIENTS; i++) {
            host_client_t *c = &host_clients[i];
            if (c->state != CLIENT_STATE_CONNECTED) continue;

            c->tx_packets++;
            c->tx_bytes += len;
            if (client_send_all(c, hdr, sizeof(hdr)))
                client_send_all(c, buf, len);
        }
    } else {
        /* Unicast to specific client */
        host_client_t *c = host_find_by_id(client_id);
        if (!c || c->state != CLIENT_STATE_CONNECTED) return;

        c->tx_packets++;
        c->tx_bytes += len;
        if (client_send_all(c, hdr, sizeof(hdr)))
            client_send_all(c, buf, len);
    }
}
