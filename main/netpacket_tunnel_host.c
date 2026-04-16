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
#include <string.h>
#include <unistd.h>

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "c6_remote.h"
#include "runtime_config.h"

extern bool netpacket_host_attach_client(int fd, const char *peer_desc);

#define TUNNEL_MAGIC_SESSION  "RATS"
#define TUNNEL_MAGIC_LINK     "RATL"
#define TUNNEL_MAGIC_PING     "RATP"
#define TUNNEL_UNIQUE_SIZE    12
#define TUNNEL_MSG_SIZE       16
#define TUNNEL_MAX_PENDING    3

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
static char ctrl_room_id[TUNNEL_UNIQUE_SIZE * 2 + 1];
static char ctrl_status[32] = "idle";
static uint8_t ctrl_recv_buf[64];
static size_t ctrl_recv_len;
static pending_link_t pending_links[TUNNEL_MAX_PENDING];

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
    ctrl_room_id[0] = '\0';
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
    ctrl_room_id[0] = '\0';
}

static bool recv_into_ctrl(void)
{
    if (ctrl_fd < 0 || ctrl_recv_len >= sizeof(ctrl_recv_buf))
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

        bytes_to_hex(ctrl_recv_buf + 4, TUNNEL_UNIQUE_SIZE,
                     ctrl_room_id, sizeof(ctrl_room_id));
        ctrl_consume(TUNNEL_MSG_SIZE);
        ctrl_state = TUNNEL_CTRL_READY;
        set_status("room_ready");
        ESP_LOGI(TAG, "Tunnel room created: %s", ctrl_room_id);
        return;
    }

    process_control_ready();
}

const char *netpacket_tunnel_host_room_id(void)
{
    return ctrl_room_id;
}

const char *netpacket_tunnel_host_status(void)
{
    return ctrl_status;
}

bool netpacket_tunnel_host_room_ready(void)
{
    return ctrl_state == TUNNEL_CTRL_READY && ctrl_room_id[0] != '\0';
}