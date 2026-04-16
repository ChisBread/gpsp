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

/* Host mode (netpacket_host.c) */
extern void netpacket_host_poll(void);
extern void netpacket_host_send(uint16_t client_id, const void *buf, size_t len);

/* ── RetroArch Netplay protocol constants ─────────────────────────── */

#define RA_NETPLAY_MAGIC             0x52414E50u  /* "RANP" */
#define RA_NETPLAY_PROTOCOL_VERSION  7u
#define RA_NICK_LEN                  32
#define RA_MAX_INPUT_DEVICES         16

/* Commands */
#define RA_CMD_NICK                  0x0020u
#define RA_CMD_INFO                  0x0022u
#define RA_CMD_SYNC                  0x0023u
#define RA_CMD_SPECTATE              0x0024u
#define RA_CMD_PLAY                  0x0025u
#define RA_CMD_MODE                  0x0026u
#define RA_CMD_MODE_REFUSED          0x0027u
#define RA_CMD_DISCONNECT            0x000Au
#define RA_CMD_NETPACKET             0x0048u
#define RA_CMD_PING_REQUEST          0x1100u
#define RA_CMD_PING_RESPONSE         0x1101u
#define RA_CMD_SETTING_ALLOW_PAUSE   0x2000u
#define RA_CMD_SETTING_INPUT_LATENCY 0x2001u

#define RA_SYNC_BIT_PAUSED           (1u << 31)

#define NETPACKET_BROADCAST          0xFFFFu
#define NETPACKET_MAX_PAYLOAD        2048u

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
static uint32_t np_server_protocol;

/* Debug counters */
static uint32_t np_tx_packets, np_tx_bytes;
static uint32_t np_rx_packets, np_rx_bytes;

/* Receive buffer for TCP stream reassembly */
#define NP_RECV_BUF_SIZE 4096
static GPSP_EXTRAM_BSS uint8_t np_recv_buf[NP_RECV_BUF_SIZE];
static size_t np_recv_len;

/* ── Helpers ──────────────────────────────────────────────────────── */

static uint32_t np_platform_magic(void)
{
    /*
     * Matches RetroArch's netplay_platform_magic():
     *   ((1 == htonl(1)) << 30) | (sizeof(size_t) << 15) | sizeof(long)
     *
     * ESP32-P4 (RISC-V 32-bit, little-endian):
     *   htonl(1) != 1  → bit 30 = 0
     *   sizeof(size_t) = 4
     *   sizeof(long)   = 4
     */
    return (0u << 30) | (sizeof(size_t) << 15) | sizeof(long);
}

static uint32_t np_impl_magic(void)
{
    const char *ver = GPSP_VERSION;
    uint32_t magic = 0;
    size_t i;

    for (i = 0; ver[i]; i++)
        magic ^= (uint32_t)ver[i] << (i & 0xf);
    magic ^= RA_NETPLAY_PROTOCOL_VERSION << (i & 0xf);

    return magic;
}

static bool np_parse_tunnel_id(const char *hex, uint8_t out[12])
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

static void np_disconnect(void)
{
    if (np_socket_fd >= 0) {
        close(np_socket_fd);
        np_socket_fd = -1;
    }
    np_state = STATE_DISCONNECTED;
    np_recv_len = 0;
    netplay_num_clients = 0;
    netplay_client_id = 0;
}

static bool np_send_all(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(np_socket_fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ESP_LOGW(TAG, "TCP send failed: errno=%d", errno);
            np_disconnect();
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

/* Non-blocking read into np_recv_buf. Returns bytes available. */
static ssize_t np_recv_more(void)
{
    if (np_recv_len >= NP_RECV_BUF_SIZE) {
        return (ssize_t)np_recv_len;
    }

    ssize_t n = recv(np_socket_fd, np_recv_buf + np_recv_len,
                     NP_RECV_BUF_SIZE - np_recv_len, MSG_DONTWAIT);
    if (n > 0) {
        np_recv_len += (size_t)n;
    } else if (n == 0) {
        /* Connection closed by peer */
        ESP_LOGW(TAG, "Connection closed by RetroArch host");
        np_disconnect();
        return -1;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "TCP recv failed: errno=%d", errno);
        np_disconnect();
        return -1;
    }

    return (ssize_t)np_recv_len;
}

/* Consume n bytes from front of np_recv_buf */
static void np_recv_consume(size_t n)
{
    if (n >= np_recv_len) {
        np_recv_len = 0;
    } else {
        memmove(np_recv_buf, np_recv_buf + n, np_recv_len - n);
        np_recv_len -= n;
    }
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

static void np_receive_dispatch(const void *buf, size_t len, uint16_t client_id)
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
        ESP_LOGW(TAG, "Dropping NETPACKET: serial_mode=%d not handled", serial_mode);
        break;
    }
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

    /* Host mode uses listener, not outbound connection */
    if (gpsp_netplay_ra_mode == NETPLAY_MODE_HOST) {
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

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(gpsp_netplay_ra_port);

    if (!inet_aton(gpsp_netplay_ra_host, &server_addr.sin_addr)) {
        ESP_LOGW(TAG, "Invalid RA host address: %s", gpsp_netplay_ra_host);
        return false;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        ESP_LOGW(TAG, "Failed to create TCP socket: errno=%d", errno);
        return false;
    }

    /* Set non-blocking for connect */
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }

    /* Disable Nagle for low latency */
    int one = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

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
            ESP_LOGE(TAG, "Invalid tunnel_id hex: %s", gpsp_netplay_ra_tunnel_id);
            np_disconnect();
            return false;
        }
        memcpy(rats_msg, "RATS", 4);
        memcpy(rats_msg + 4, session_bytes, 12);
        if (!np_send_all(rats_msg, sizeof(rats_msg))) {
            return false;
        }
        ESP_LOGI(TAG, "Sent tunnel RATS: session=%s", gpsp_netplay_ra_tunnel_id);
    }

    /* Send our header immediately */
    uint32_t header[6];
    header[0] = htonl(RA_NETPLAY_MAGIC);
    header[1] = htonl(np_platform_magic());
    header[2] = htonl(0);  /* No compression */
    header[3] = htonl(RA_NETPLAY_PROTOCOL_VERSION);  /* Highest supported */
    header[4] = htonl(0);  /* Negotiate */
    header[5] = htonl(np_impl_magic());

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
    if (salt != 0) {
        ESP_LOGE(TAG, "Server requires password — not supported on ESP32-P4");
        np_disconnect();
        return false;
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
            /* Respond with PING_RESPONSE */
            uint32_t pong[2];
            pong[0] = htonl(RA_CMD_PING_RESPONSE);
            pong[1] = htonl(0);
            np_send_all(pong, sizeof(pong));
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
    /* Host mode management (also handles cleanup on mode change) */
    netpacket_host_poll();
    if (gpsp_netplay_ra_mode == NETPLAY_MODE_HOST) {
        return;
    }

    if (np_state == STATE_DISCONNECTED) {
        if (gpsp_netplay_ra_enabled) {
            np_start_connect();
        }
        return;
    }

    /* Try to receive data */
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
                np_send_all(ping, sizeof(ping));
                np_last_ping_us = now;

                if (np_tx_packets || np_rx_packets) {
                    ESP_LOGI(TAG, "Stats: TX %u pkts/%u B, RX %u pkts/%u B",
                             np_tx_packets, np_tx_bytes,
                             np_rx_packets, np_rx_bytes);
                    np_tx_packets = np_tx_bytes = 0;
                    np_rx_packets = np_rx_bytes = 0;
                }
            }
        }
        break;
    default:
        break;
    }
}

void netpacket_send(uint16_t client_id, const void *buf, size_t len)
{
    if (gpsp_netplay_ra_mode == NETPLAY_MODE_HOST) {
        netpacket_host_send(client_id, buf, len);
        return;
    }

    if (np_state != STATE_CONNECTED || np_socket_fd < 0 || !buf || len == 0) {
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
     */
    uint32_t hdr[3];
    hdr[0] = htonl(RA_CMD_NETPACKET);
    hdr[1] = htonl((uint32_t)len);
    hdr[2] = htonl((uint32_t)client_id);

    np_tx_packets++;
    np_tx_bytes += len;
    ESP_LOGD(TAG, "NETPACKET send: target=%u len=%u", (unsigned)client_id, (unsigned)len);

    if (!np_send_all(hdr, sizeof(hdr))) {
        return;
    }
    np_send_all(buf, len);
}
