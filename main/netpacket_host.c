/*
 * gpsp app support — RetroArch netplay TCP HOST for ESP32-P4
 *
 * Implements the host (server) side of the RetroArch Netplay protocol,
 * allowing other devices (RA clients or other ESP32-P4 units) to connect
 * for multiplayer gameplay.  Currently supports a single connected client.
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

#define NETPACKET_MAX_PAYLOAD        2048u

/* ── Host states ──────────────────────────────────────────────────── */

typedef enum {
    HOST_STATE_IDLE = 0,
    HOST_STATE_LISTENING,
    HOST_STATE_WAIT_CLIENT_HEADER,
    HOST_STATE_WAIT_CLIENT_NICK,
    HOST_STATE_WAIT_CLIENT_INFO,
    HOST_STATE_WAIT_PLAY,
    HOST_STATE_CONNECTED,
} host_state_t;

/* ── Module state ─────────────────────────────────────────────────── */

extern u32 netplay_num_clients;
extern u32 netplay_client_id;

static const char *TAG = "gpsp_host";

static int host_listen_fd  = -1;
static int host_client_fd  = -1;
static host_state_t host_state = HOST_STATE_IDLE;

static int64_t host_last_ping_us;
static uint32_t host_client_protocol;

/* Debug counters */
static uint32_t host_tx_packets, host_tx_bytes;
static uint32_t host_rx_packets, host_rx_bytes;

/* Receive buffer for TCP stream reassembly */
#define HOST_RECV_BUF_SIZE 4096
static GPSP_EXTRAM_BSS uint8_t host_recv_buf[HOST_RECV_BUF_SIZE];
static size_t host_recv_len;

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

static void host_disconnect_client(void)
{
    if (host_client_fd >= 0) {
        close(host_client_fd);
        host_client_fd = -1;
    }
    host_recv_len = 0;
    netplay_num_clients = 0;

    /* Reset serial protocol state machines */
    serialproto_reset();
    rfu_reset();
    serial_reset_irq();

    /* Go back to listening if listener is still alive */
    if (host_listen_fd >= 0) {
        host_state = HOST_STATE_LISTENING;
        ESP_LOGI(TAG, "Client disconnected, back to listening");
    } else {
        host_state = HOST_STATE_IDLE;
    }
}

static void host_stop(void)
{
    if (host_client_fd >= 0) {
        close(host_client_fd);
        host_client_fd = -1;
    }
    if (host_listen_fd >= 0) {
        close(host_listen_fd);
        host_listen_fd = -1;
    }
    host_state = HOST_STATE_IDLE;
    host_recv_len = 0;
    netplay_num_clients = 0;
    netplay_client_id = 0;

    /* Reset serial protocol state machines */
    serialproto_reset();
    rfu_reset();
    serial_reset_irq();
}

static bool host_send_all(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(host_client_fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            ESP_LOGW(TAG, "TCP send failed: errno=%d", errno);
            host_disconnect_client();
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static ssize_t host_recv_more(void)
{
    if (host_recv_len >= HOST_RECV_BUF_SIZE)
        return (ssize_t)host_recv_len;

    ssize_t n = recv(host_client_fd, host_recv_buf + host_recv_len,
                     HOST_RECV_BUF_SIZE - host_recv_len, MSG_DONTWAIT);
    if (n > 0) {
        host_recv_len += (size_t)n;
    } else if (n == 0) {
        ESP_LOGW(TAG, "Client disconnected");
        host_disconnect_client();
        return -1;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "TCP recv failed: errno=%d", errno);
        host_disconnect_client();
        return -1;
    }
    return (ssize_t)host_recv_len;
}

static void host_recv_consume(size_t n)
{
    if (n >= host_recv_len)
        host_recv_len = 0;
    else {
        memmove(host_recv_buf, host_recv_buf + n, host_recv_len - n);
        host_recv_len -= n;
    }
}

static bool host_try_read(void *out, size_t needed)
{
    if (host_recv_len < needed) {
        if (host_recv_more() < 0) return false;
    }
    if (host_recv_len < needed) return false;
    memcpy(out, host_recv_buf, needed);
    host_recv_consume(needed);
    return true;
}

/* ── Receive dispatch ─────────────────────────────────────────────── */

static void host_receive_dispatch(const void *buf, size_t len, uint16_t client_id)
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

/* ── Handshake phases (reversed: host receives first, then responds) */

static bool host_start_listen(void)
{
    if (host_listen_fd >= 0) return true;

    if (!c6_remote_network_ready()) {
        return false;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        ESP_LOGW(TAG, "Failed to create listen socket: errno=%d", errno);
        return false;
    }

    int one = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    /* Non-blocking for accept() */
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags >= 0) fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(gpsp_netplay_ra_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sockfd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "Bind failed on port %u: errno=%d",
                 gpsp_netplay_ra_port, errno);
        close(sockfd);
        return false;
    }

    if (listen(sockfd, 1) < 0) {
        ESP_LOGW(TAG, "Listen failed: errno=%d", errno);
        close(sockfd);
        return false;
    }

    host_listen_fd = sockfd;
    host_state = HOST_STATE_LISTENING;
    netplay_client_id = 0;  /* Host is always client 0 */
    ESP_LOGI(TAG, "Listening for netplay clients on port %u",
             gpsp_netplay_ra_port);
    return true;
}

static bool host_check_accept(void)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int fd = accept(host_listen_fd,
                    (struct sockaddr *)&client_addr, &addr_len);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        ESP_LOGW(TAG, "Accept failed: errno=%d", errno);
        return false;
    }

    /* Non-blocking + TCP_NODELAY */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    host_client_fd = fd;
    host_recv_len = 0;

    char addr_str[INET_ADDRSTRLEN];
    inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str));
    ESP_LOGI(TAG, "Client connected from %s:%u",
             addr_str, ntohs(client_addr.sin_port));

    host_state = HOST_STATE_WAIT_CLIENT_HEADER;
    return true;
}

static bool host_handle_client_header(void)
{
    uint32_t header[6];
    if (!host_try_read(header, sizeof(header))) return false;

    if (ntohl(header[0]) != RA_NETPLAY_MAGIC) {
        ESP_LOGE(TAG, "Bad magic from client: 0x%08x", ntohl(header[0]));
        host_disconnect_client();
        return false;
    }

    uint32_t client_proto = ntohl(header[3]);
    ESP_LOGI(TAG, "Client protocol: %u, platform: 0x%08x",
             (unsigned)client_proto, ntohl(header[1]));

    /* Negotiate protocol version */
    host_client_protocol = client_proto < RA_NETPLAY_PROTOCOL_VERSION
                             ? client_proto : RA_NETPLAY_PROTOCOL_VERSION;
    if (host_client_protocol < 5) {
        ESP_LOGE(TAG, "Client protocol too old: %u", (unsigned)client_proto);
        host_disconnect_client();
        return false;
    }

    /* Send our header in response */
    uint32_t response[6];
    response[0] = htonl(RA_NETPLAY_MAGIC);
    response[1] = htonl(host_platform_magic());
    response[2] = htonl(0);                        /* No compression */
    response[3] = htonl(0);                        /* No password */
    response[4] = htonl(host_client_protocol);     /* Negotiated version */
    response[5] = htonl(host_impl_magic());

    if (!host_send_all(response, sizeof(response))) return false;

    host_state = HOST_STATE_WAIT_CLIENT_NICK;
    return true;
}

static bool host_handle_client_nick(void)
{
    uint32_t cmd[2];
    if (!host_try_read(cmd, sizeof(cmd))) return false;

    if (ntohl(cmd[0]) != RA_CMD_NICK || ntohl(cmd[1]) != RA_NICK_LEN) {
        ESP_LOGE(TAG, "Expected NICK, got cmd=0x%04x size=%u",
                 ntohl(cmd[0]), ntohl(cmd[1]));
        host_disconnect_client();
        return false;
    }

    char nick[RA_NICK_LEN];
    if (!host_try_read(nick, sizeof(nick))) {
        host_disconnect_client();
        return false;
    }
    nick[RA_NICK_LEN - 1] = '\0';
    ESP_LOGI(TAG, "Client nick: \"%s\"", nick);

    /* Send our NICK */
    uint32_t nick_cmd[2];
    char our_nick[RA_NICK_LEN];
    memset(our_nick, 0, sizeof(our_nick));
    strlcpy(our_nick, gpsp_netplay_ra_nick, sizeof(our_nick));

    nick_cmd[0] = htonl(RA_CMD_NICK);
    nick_cmd[1] = htonl(RA_NICK_LEN);

    if (!host_send_all(nick_cmd, sizeof(nick_cmd)) ||
        !host_send_all(our_nick, sizeof(our_nick))) {
        return false;
    }

    host_state = HOST_STATE_WAIT_CLIENT_INFO;
    return true;
}

static bool host_handle_client_info(void)
{
    uint32_t cmd[2];
    if (!host_try_read(cmd, sizeof(cmd))) return false;

    uint32_t cmd_id   = ntohl(cmd[0]);
    uint32_t cmd_size = ntohl(cmd[1]);

    if (cmd_id != RA_CMD_INFO) {
        ESP_LOGE(TAG, "Expected INFO, got cmd=0x%04x", cmd_id);
        host_disconnect_client();
        return false;
    }

    /* Consume client INFO payload */
    while (cmd_size > 0) {
        uint8_t discard[256];
        size_t chunk = cmd_size < sizeof(discard) ? cmd_size : sizeof(discard);
        if (!host_try_read(discard, chunk)) {
            host_disconnect_client();
            return false;
        }
        cmd_size -= chunk;
    }

    /* ── Send our INFO ──────────────────────────────────────────── */
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
    strlcpy(info.core_version, GPSP_NETPACKET_VERSION, sizeof(info.core_version));

    if (!host_send_all(&info, sizeof(info))) return false;

    /* ── Send SYNC (assign client_id = 1) ───────────────────────── */
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

    uint32_t assigned_id = 1;

    sync.cmd[0] = htonl(RA_CMD_SYNC);
    sync.cmd[1] = htonl(sizeof(sync) - sizeof(sync.cmd));
    sync.frame_count = htonl(0);
    sync.client_num  = htonl(assigned_id);
    /* devices[]: all zero (CORE_PACKET_INTERFACE, game handles mapping) */
    /* share_modes[]: all zero */
    sync.device_clients[0] = htonl(0);           /* slot 0 → host */
    sync.device_clients[1] = htonl(assigned_id); /* slot 1 → client */
    strlcpy(sync.nick, gpsp_netplay_ra_nick, sizeof(sync.nick));

    if (!host_send_all(&sync, sizeof(sync))) return false;

    ESP_LOGI(TAG, "Sent INFO + SYNC, assigned client_id=%u",
             (unsigned)assigned_id);

    host_state = HOST_STATE_WAIT_PLAY;
    return true;
}

static bool host_handle_play(void)
{
    uint32_t cmd[2];
    if (!host_try_read(cmd, sizeof(cmd))) return false;

    uint32_t cmd_id   = ntohl(cmd[0]);
    uint32_t cmd_size = ntohl(cmd[1]);

    if (cmd_id != RA_CMD_PLAY) {
        /* Skip non-PLAY commands (SETTING etc.) that arrive before PLAY */
        ESP_LOGD(TAG, "Pre-PLAY cmd 0x%04x size %u, skipping",
                 cmd_id, (unsigned)cmd_size);
        while (cmd_size > 0) {
            uint8_t discard[256];
            size_t chunk = cmd_size < sizeof(discard) ? cmd_size : sizeof(discard);
            if (!host_try_read(discard, chunk)) {
                host_disconnect_client();
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
        if (!host_try_read(discard, chunk)) {
            host_disconnect_client();
            return false;
        }
        cmd_size -= chunk;
    }

    ESP_LOGI(TAG, "Client requested PLAY");

    /* Send MODE to confirm: "you (client 1) are now playing" */
    uint32_t mode_msg[5];
    uint32_t assigned_id = 1;

    mode_msg[0] = htonl(RA_CMD_MODE);
    mode_msg[1] = htonl(12);                          /* 3 × uint32_t */
    mode_msg[2] = htonl(0);                            /* frame */
    mode_msg[3] = htonl((1u << 31) | (1u << 30));     /* is_you | is_playing */
    mode_msg[4] = htonl(assigned_id);

    if (!host_send_all(mode_msg, sizeof(mode_msg))) return false;

    netplay_client_id = 0;  /* host is always 0 */
    netplay_num_clients = 1;
    host_last_ping_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Client %u now PLAYING — host ready!",
             (unsigned)assigned_id);

    host_state = HOST_STATE_CONNECTED;
    return true;
}

/* ── Runtime command processing (host side) ───────────────────────── */

static void host_process_commands(void)
{
    while (host_recv_len >= 8 && host_state == HOST_STATE_CONNECTED) {
        uint32_t cmd_hdr[2];
        memcpy(cmd_hdr, host_recv_buf, sizeof(cmd_hdr));
        uint32_t cmd_id   = ntohl(cmd_hdr[0]);
        uint32_t cmd_size = ntohl(cmd_hdr[1]);

        size_t total_needed;
        if (cmd_id == RA_CMD_NETPACKET)
            total_needed = 8 + 4 + cmd_size;   /* hdr + client_id + data */
        else
            total_needed = 8 + cmd_size;

        if (host_recv_len < total_needed) break;

        uint8_t *payload = host_recv_buf + 8;

        switch (cmd_id) {
        case RA_CMD_NETPACKET: {
            uint32_t sender_id;
            memcpy(&sender_id, payload, sizeof(sender_id));
            sender_id = ntohl(sender_id);

            const void *pkt_data = payload + 4;
            size_t pkt_len = cmd_size;

            host_rx_packets++;
            host_rx_bytes += pkt_len;

            host_receive_dispatch(pkt_data, pkt_len, (uint16_t)sender_id);
            break;
        }

        case RA_CMD_PING_REQUEST: {
            uint32_t pong[2];
            pong[0] = htonl(RA_CMD_PING_RESPONSE);
            pong[1] = htonl(0);
            host_send_all(pong, sizeof(pong));
            break;
        }

        case RA_CMD_PING_RESPONSE:
            break;

        case RA_CMD_DISCONNECT:
            ESP_LOGW(TAG, "Client sent DISCONNECT");
            host_disconnect_client();
            return;

        default:
            ESP_LOGD(TAG, "Ignoring cmd 0x%04x size %u",
                     cmd_id, (unsigned)cmd_size);
            break;
        }

        host_recv_consume(total_needed);
    }
}

/* ── Public API (called from netpacket_bridge_ra.c) ───────────────── */

void netpacket_host_poll(void)
{
    /* If mode is not HOST, clean up any leftover state and return */
    if (gpsp_netplay_ra_mode != NETPLAY_MODE_HOST) {
        if (host_state != HOST_STATE_IDLE)
            host_stop();
        return;
    }

    if (host_state == HOST_STATE_IDLE) {
        host_start_listen();
        return;
    }

    /* Read from client socket if connected */
    if (host_client_fd >= 0) {
        host_recv_more();
    }

    switch (host_state) {
    case HOST_STATE_LISTENING:
        host_check_accept();
        break;
    case HOST_STATE_WAIT_CLIENT_HEADER:
        host_handle_client_header();
        break;
    case HOST_STATE_WAIT_CLIENT_NICK:
        host_handle_client_nick();
        break;
    case HOST_STATE_WAIT_CLIENT_INFO:
        host_handle_client_info();
        break;
    case HOST_STATE_WAIT_PLAY:
        host_handle_play();
        break;
    case HOST_STATE_CONNECTED:
        host_process_commands();

        /* Periodic ping + stats (every 5 seconds) */
        if (host_client_fd >= 0) {
            int64_t now = esp_timer_get_time();
            if (now - host_last_ping_us > 5000000) {
                uint32_t ping[2];
                ping[0] = htonl(RA_CMD_PING_REQUEST);
                ping[1] = htonl(0);
                host_send_all(ping, sizeof(ping));
                host_last_ping_us = now;

                if (host_tx_packets || host_rx_packets) {
                    ESP_LOGI(TAG, "Stats: TX %u pkts/%u B, RX %u pkts/%u B",
                             host_tx_packets, host_tx_bytes,
                             host_rx_packets, host_rx_bytes);
                    host_tx_packets = host_tx_bytes = 0;
                    host_rx_packets = host_rx_bytes = 0;
                }
            }
        }
        break;
    default:
        break;
    }
}

void netpacket_host_send(uint16_t client_id, const void *buf, size_t len)
{
    if (host_state != HOST_STATE_CONNECTED || host_client_fd < 0 ||
        !buf || len == 0) {
        return;
    }

    if (len > NETPACKET_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "Dropping oversized netpacket (%u bytes)",
                 (unsigned)len);
        return;
    }

    uint32_t hdr[3];
    hdr[0] = htonl(RA_CMD_NETPACKET);
    hdr[1] = htonl((uint32_t)len);
    hdr[2] = htonl((uint32_t)client_id);

    host_tx_packets++;
    host_tx_bytes += len;

    if (!host_send_all(hdr, sizeof(hdr))) return;
    host_send_all(buf, len);
}
