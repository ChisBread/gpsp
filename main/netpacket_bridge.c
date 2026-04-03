/*
 * gpsp app support — host-side netpacket transport for ESP32-P4
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
#include "esp_random.h"
#include "esp_timer.h"

#include "c6_remote.h"
#include "common.h"
#include "gpsp_config.h"
#include "main.h"
#include "runtime_config.h"
#include "serial.h"

#define GPSP_NETPACKET_MAGIC 0x47504E50u
#define GPSP_NETPACKET_WIRE_VERSION 0x0001u
#define GPSP_NETPACKET_MAX_PAYLOAD 512u
#define GPSP_NETPACKET_BROADCAST 0xFFFFu
#define GPSP_NETPACKET_TYPE_HELLO 0x01u
#define GPSP_NETPACKET_TYPE_DATA  0x02u
#define GPSP_NETPACKET_CLIENT_ID_AUTO 0xFFu

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t packet_type;
    uint8_t packet_serial_mode;
    uint8_t preferred_client_id;
    uint8_t reserved0;
    uint32_t node_uid_hi;
    uint32_t node_uid_lo;
    uint16_t dst_client_id;
    uint16_t payload_len;
    uint8_t payload[GPSP_NETPACKET_MAX_PAYLOAD];
} gpsp_netpacket_wire_t;

typedef struct {
    bool active;
    int64_t last_seen_us;
    uint64_t node_uid;
    uint8_t packet_serial_mode;
    uint8_t preferred_client_id;
    struct sockaddr_in addr;
} netpacket_peer_state_t;

u32 netplay_num_clients = 0;
u32 netplay_client_id = 0;

#if CONFIG_GPSP_NETPLAY_UDP_ENABLE
static const char *TAG = "gpsp_netpacket";
static int netpacket_socket_fd = -1;
static int64_t netpacket_last_open_attempt_us;
static int64_t netpacket_last_hello_us;
static bool netpacket_ready_logged;
static bool netpacket_local_unassigned_logged;
static struct sockaddr_in netpacket_broadcast_addr;
static netpacket_peer_state_t netpacket_peers[MAX_RFU_NETPLAYERS];
static uint64_t netpacket_local_node_uid;
static bool netpacket_local_assigned;

static uint8_t netpacket_local_preferred_client_id(void)
{
    if (gpsp_netplay_local_client_id_override < 0 ||
        gpsp_netplay_local_client_id_override > UINT8_MAX) {
        return GPSP_NETPACKET_CLIENT_ID_AUTO;
    }

    return (uint8_t)gpsp_netplay_local_client_id_override;
}

static uint32_t netpacket_peer_timeout_ms(void)
{
    if (gpsp_netplay_peer_timeout_ms < 100) {
        return 100;
    }

    return gpsp_netplay_peer_timeout_ms;
}

static uint32_t netpacket_hello_interval_ms(void)
{
    if (gpsp_netplay_hello_interval_ms < 50) {
        return 50;
    }

    return gpsp_netplay_hello_interval_ms;
}

static uint16_t netpacket_udp_port(void)
{
    if (gpsp_netplay_udp_port < 1024) {
        return 1024;
    }

    return gpsp_netplay_udp_port;
}

static const char *netpacket_broadcast_ip(void)
{
    if (gpsp_netplay_broadcast_addr[0] == '\0') {
        return "255.255.255.255";
    }

    return gpsp_netplay_broadcast_addr;
}

static bool netpacket_runtime_enabled(void)
{
    return gpsp_netplay_udp_enabled;
}

static uint64_t netpacket_wire_node_uid(const gpsp_netpacket_wire_t *packet)
{
    return ((uint64_t)ntohl(packet->node_uid_hi) << 32) | ntohl(packet->node_uid_lo);
}

static void netpacket_fill_node_uid(gpsp_netpacket_wire_t *packet, uint64_t node_uid)
{
    packet->node_uid_hi = htonl((uint32_t)(node_uid >> 32));
    packet->node_uid_lo = htonl((uint32_t)(node_uid & 0xFFFFFFFFu));
}

static uint64_t netpacket_local_uid(void)
{
    if (netpacket_local_node_uid == 0) {
        netpacket_local_node_uid = ((uint64_t)esp_random() << 32) | esp_random();
        if (netpacket_local_node_uid == 0) {
            netpacket_local_node_uid = 1;
        }
    }

    return netpacket_local_node_uid;
}

static uint16_t netpacket_total_slots(void)
{
    switch (serial_mode) {
    case SERIAL_MODE_RFU:
        return MAX_RFU_NETPLAYERS;
    case SERIAL_MODE_SERIAL_POKE:
    case SERIAL_MODE_SERIAL_AW1:
    case SERIAL_MODE_SERIAL_AW2:
        return MAX_SERMULT_NETPLAYERS;
    default:
        return 0;
    }
}

static void netpacket_sort_uids(uint64_t *uids, size_t count)
{
    for (size_t index = 1; index < count; index++) {
        uint64_t value = uids[index];
        size_t insert = index;

        while (insert > 0 && uids[insert - 1] > value) {
            uids[insert] = uids[insert - 1];
            insert--;
        }
        uids[insert] = value;
    }
}

static void netpacket_expire_peers(int64_t now_us)
{
    int64_t timeout_us = (int64_t)netpacket_peer_timeout_ms() * 1000;

    for (uint16_t index = 0; index < MAX_RFU_NETPLAYERS; index++) {
        netpacket_peer_state_t *peer = &netpacket_peers[index];

        if (!peer->active) {
            continue;
        }

        if ((now_us - peer->last_seen_us) > timeout_us) {
            peer->active = false;
        }
    }
}

static size_t netpacket_collect_roster(uint64_t *uids, size_t capacity)
{
    size_t count = 0;
    uint64_t local_uid = netpacket_local_uid();

    if (capacity == 0) {
        return 0;
    }

    uids[count++] = local_uid;
    for (uint16_t index = 0; index < MAX_RFU_NETPLAYERS && count < capacity; index++) {
        netpacket_peer_state_t *peer = &netpacket_peers[index];

        if (!peer->active || peer->node_uid == local_uid || peer->packet_serial_mode != serial_mode) {
            continue;
        }

        uids[count++] = peer->node_uid;
    }

    netpacket_sort_uids(uids, count);
    return count;
}

static bool netpacket_node_preferred_client_id(uint64_t node_uid, uint16_t *out_client_id)
{
    uint16_t total_slots = netpacket_total_slots();

    if (node_uid == netpacket_local_uid()) {
        uint8_t preferred_client_id = netpacket_local_preferred_client_id();

        if (preferred_client_id != GPSP_NETPACKET_CLIENT_ID_AUTO && preferred_client_id < total_slots) {
            *out_client_id = preferred_client_id;
            return true;
        }

        return false;
    }

    for (uint16_t index = 0; index < MAX_RFU_NETPLAYERS; index++) {
        netpacket_peer_state_t *peer = &netpacket_peers[index];

        if (!peer->active || peer->node_uid != node_uid || peer->packet_serial_mode != serial_mode) {
            continue;
        }

        if (peer->preferred_client_id != GPSP_NETPACKET_CLIENT_ID_AUTO &&
            peer->preferred_client_id < total_slots) {
            *out_client_id = peer->preferred_client_id;
            return true;
        }

        return false;
    }

    return false;
}

static size_t netpacket_build_assignment(uint64_t *assigned_node_uids, size_t capacity)
{
    uint16_t total_slots = netpacket_total_slots();
    uint64_t roster[MAX_RFU_NETPLAYERS];
    bool assigned[MAX_RFU_NETPLAYERS] = { false };
    size_t roster_count;
    size_t slot_count;
    size_t assigned_count = 0;

    if (total_slots == 0 || capacity == 0) {
        return 0;
    }

    slot_count = total_slots < capacity ? total_slots : capacity;
    memset(assigned_node_uids, 0, slot_count * sizeof(*assigned_node_uids));
    roster_count = netpacket_collect_roster(roster, MAX_RFU_NETPLAYERS);

    for (size_t index = 0; index < roster_count && assigned_count < slot_count; index++) {
        uint16_t preferred_client_id;

        if (!netpacket_node_preferred_client_id(roster[index], &preferred_client_id) ||
            preferred_client_id >= slot_count || assigned_node_uids[preferred_client_id] != 0) {
            continue;
        }

        assigned_node_uids[preferred_client_id] = roster[index];
        assigned[index] = true;
        assigned_count++;
    }

    for (size_t index = 0; index < roster_count && assigned_count < slot_count; index++) {
        if (assigned[index]) {
            continue;
        }

        for (size_t slot = 0; slot < slot_count; slot++) {
            if (assigned_node_uids[slot] == 0) {
                assigned_node_uids[slot] = roster[index];
                assigned_count++;
                break;
            }
        }
    }

    return assigned_count;
}

static bool netpacket_find_assigned_client_id(uint64_t node_uid, uint16_t *out_client_id)
{
    uint64_t assigned_node_uids[MAX_RFU_NETPLAYERS];
    size_t assigned_count = netpacket_build_assignment(assigned_node_uids, MAX_RFU_NETPLAYERS);

    if (assigned_count == 0) {
        return false;
    }

    for (size_t index = 0; index < assigned_count; index++) {
        if (assigned_node_uids[index] == node_uid) {
            *out_client_id = (uint16_t)index;
            return true;
        }
    }

    return false;
}

static netpacket_peer_state_t *netpacket_find_peer_for_client_id(uint16_t client_id)
{
    for (uint16_t index = 0; index < MAX_RFU_NETPLAYERS; index++) {
        netpacket_peer_state_t *peer = &netpacket_peers[index];
        uint16_t peer_client_id;

        if (!peer->active || peer->packet_serial_mode != serial_mode) {
            continue;
        }

        if (netpacket_find_assigned_client_id(peer->node_uid, &peer_client_id) &&
            peer_client_id == client_id) {
            return peer;
        }
    }

    return NULL;
}

static void netpacket_refresh_client_count(int64_t now_us)
{
    uint16_t total_slots = netpacket_total_slots();
    uint64_t assigned_node_uids[MAX_RFU_NETPLAYERS];
    size_t assigned_count;
    uint16_t local_client_id;

    netpacket_expire_peers(now_us);
    netplay_num_clients = 0;
    netplay_client_id = 0;
    netpacket_local_assigned = false;

    if (total_slots == 0) {
        netpacket_local_unassigned_logged = false;
        return;
    }

    assigned_count = netpacket_build_assignment(assigned_node_uids, MAX_RFU_NETPLAYERS);

    if (netpacket_find_assigned_client_id(netpacket_local_uid(), &local_client_id)) {
        netplay_client_id = local_client_id;
        netplay_num_clients = assigned_count > 0 ? (u32)(assigned_count - 1U) : 0;
        netpacket_local_assigned = true;
        netpacket_local_unassigned_logged = false;
        return;
    }

    if (!netpacket_local_unassigned_logged) {
        ESP_LOGW(TAG,
                 "Local node was not selected into the active roster (serial mode %d, slots=%u, peers_seen=%u)",
                 serial_mode,
                 (unsigned)total_slots,
                 (unsigned)assigned_count);
        netpacket_local_unassigned_logged = true;
    }
}

static void netpacket_receive_dispatch(const void *buf, size_t len, uint16_t client_id)
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

static bool netpacket_transport_supported(void)
{
    return netpacket_total_slots() > 0 && netpacket_runtime_enabled();
}

static void netpacket_reset_runtime_state(void)
{
    if (netpacket_socket_fd >= 0) {
        netpacket_close_socket();
    }

    memset(netpacket_peers, 0, sizeof(netpacket_peers));
    netplay_num_clients = 0;
    netplay_client_id = 0;
    netpacket_local_assigned = false;
    netpacket_local_unassigned_logged = false;
    netpacket_last_hello_us = 0;
}

static bool netpacket_runtime_ready(void)
{
    if (!netpacket_transport_supported() || !c6_remote_network_ready()) {
        netpacket_reset_runtime_state();
        return false;
    }

    return true;
}

static void netpacket_close_socket(void)
{
    if (netpacket_socket_fd >= 0) {
        close(netpacket_socket_fd);
        netpacket_socket_fd = -1;
    }
    netpacket_ready_logged = false;
}

static esp_err_t netpacket_open_socket(void)
{
    int sockfd;
    int one = 1;
    int flags;
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(netpacket_udp_port()),
        .sin_addr = {
            .s_addr = htonl(INADDR_ANY),
        },
    };

    if (netpacket_socket_fd >= 0) {
        return ESP_OK;
    }

    if (!netpacket_transport_supported()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    int64_t now_us = esp_timer_get_time();
    if (now_us - netpacket_last_open_attempt_us < 1000000) {
        return ESP_ERR_INVALID_STATE;
    }
    netpacket_last_open_attempt_us = now_us;

    memset(&netpacket_broadcast_addr, 0, sizeof(netpacket_broadcast_addr));
    netpacket_broadcast_addr.sin_family = AF_INET;
    netpacket_broadcast_addr.sin_port = htons(netpacket_udp_port());

    if (!inet_aton(netpacket_broadcast_ip(), &netpacket_broadcast_addr.sin_addr)) {
        ESP_LOGW(TAG, "Invalid netplay broadcast address: %s", netpacket_broadcast_ip());
        return ESP_ERR_INVALID_ARG;
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sockfd < 0) {
        ESP_LOGW(TAG, "Failed to create netplay UDP socket: errno=%d", errno);
        return ESP_FAIL;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0 ||
        setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0) {
        ESP_LOGW(TAG, "Failed to configure netplay UDP socket: errno=%d", errno);
        close(sockfd);
        return ESP_FAIL;
    }

    flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "Failed to set netplay UDP socket nonblocking: errno=%d", errno);
        close(sockfd);
        return ESP_FAIL;
    }

    if (bind(sockfd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGW(TAG, "Failed to bind netplay UDP socket on port %d: errno=%d",
                 netpacket_udp_port(),
                 errno);
        close(sockfd);
        return ESP_FAIL;
    }

    netpacket_socket_fd = sockfd;
    if (!netpacket_ready_logged) {
        ESP_LOGI(TAG,
                 "Host-side netplay ready: client=%u udp_port=%d broadcast=%s",
                 (unsigned)netplay_client_id,
                 netpacket_udp_port(),
                 netpacket_broadcast_ip());
        netpacket_ready_logged = true;
    }
    return ESP_OK;
}

static netpacket_peer_state_t *netpacket_find_peer(uint64_t node_uid)
{
    for (uint16_t index = 0; index < MAX_RFU_NETPLAYERS; index++) {
        if (netpacket_peers[index].active && netpacket_peers[index].node_uid == node_uid) {
            return &netpacket_peers[index];
        }
    }

    return NULL;
}

static netpacket_peer_state_t *netpacket_allocate_peer(uint64_t node_uid)
{
    netpacket_peer_state_t *free_slot = NULL;
    netpacket_peer_state_t *oldest_slot = NULL;

    for (uint16_t index = 0; index < MAX_RFU_NETPLAYERS; index++) {
        netpacket_peer_state_t *peer = &netpacket_peers[index];

        if (!peer->active) {
            free_slot = peer;
            break;
        }

        if (!oldest_slot || peer->last_seen_us < oldest_slot->last_seen_us) {
            oldest_slot = peer;
        }
    }

    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->node_uid = node_uid;
        return free_slot;
    }

    if (oldest_slot) {
        memset(oldest_slot, 0, sizeof(*oldest_slot));
        oldest_slot->node_uid = node_uid;
    }

    return oldest_slot;
}

static bool netpacket_accept_wire_packet(const gpsp_netpacket_wire_t *packet,
                                         size_t packet_size,
                                         uint64_t *out_node_uid,
                                         size_t *out_payload_len)
{
    uint16_t payload_len;
    uint64_t node_uid;

    if (packet_size < offsetof(gpsp_netpacket_wire_t, payload)) {
        return false;
    }

    if (ntohl(packet->magic) != GPSP_NETPACKET_MAGIC ||
        ntohs(packet->version) != GPSP_NETPACKET_WIRE_VERSION) {
        return false;
    }

    if (packet->packet_serial_mode != serial_mode) {
        return false;
    }

    if (packet->packet_type != GPSP_NETPACKET_TYPE_HELLO &&
        packet->packet_type != GPSP_NETPACKET_TYPE_DATA) {
        return false;
    }

    if (packet->preferred_client_id != GPSP_NETPACKET_CLIENT_ID_AUTO &&
        packet->preferred_client_id >= netpacket_total_slots()) {
        return false;
    }

    payload_len = ntohs(packet->payload_len);
    node_uid = netpacket_wire_node_uid(packet);

    if (node_uid == 0 || node_uid == netpacket_local_uid()) {
        return false;
    }

    if (payload_len > GPSP_NETPACKET_MAX_PAYLOAD ||
        packet_size != offsetof(gpsp_netpacket_wire_t, payload) + payload_len) {
        return false;
    }

    if (packet->packet_type == GPSP_NETPACKET_TYPE_HELLO && payload_len != 0) {
        return false;
    }

    if (packet->packet_type == GPSP_NETPACKET_TYPE_DATA && payload_len == 0) {
        return false;
    }

    *out_node_uid = node_uid;
    *out_payload_len = payload_len;
    return true;
}

static void netpacket_mark_peer_seen(uint64_t node_uid,
                                     const struct sockaddr_in *src_addr,
                                     uint8_t packet_serial_mode,
                                     uint8_t preferred_client_id,
                                     int64_t now_us)
{
    netpacket_peer_state_t *peer = netpacket_find_peer(node_uid);

    if (!peer) {
        peer = netpacket_allocate_peer(node_uid);
    }

    if (!peer) {
        return;
    }

    peer->active = true;
    peer->node_uid = node_uid;
    peer->packet_serial_mode = packet_serial_mode;
    peer->preferred_client_id = preferred_client_id;
    peer->last_seen_us = now_us;
    if (src_addr) {
        peer->addr = *src_addr;
    }
}

static void netpacket_maybe_send_hello(int64_t now_us)
{
    gpsp_netpacket_wire_t packet = {
        .magic = htonl(GPSP_NETPACKET_MAGIC),
        .version = htons(GPSP_NETPACKET_WIRE_VERSION),
        .packet_type = GPSP_NETPACKET_TYPE_HELLO,
        .packet_serial_mode = (uint8_t)serial_mode,
        .preferred_client_id = netpacket_local_preferred_client_id(),
        .reserved0 = 0,
        .dst_client_id = htons(GPSP_NETPACKET_BROADCAST),
        .payload_len = htons(0),
    };

    if (!netpacket_transport_supported()) {
        return;
    }

    if (now_us - netpacket_last_hello_us < (int64_t)netpacket_hello_interval_ms() * 1000) {
        return;
    }

    if (netpacket_open_socket() != ESP_OK) {
        return;
    }

    netpacket_fill_node_uid(&packet, netpacket_local_uid());
    if (sendto(netpacket_socket_fd,
               &packet,
               offsetof(gpsp_netpacket_wire_t, payload),
               0,
               (const struct sockaddr *)&netpacket_broadcast_addr,
               sizeof(netpacket_broadcast_addr)) < 0) {
        ESP_LOGW(TAG, "Netplay hello send failed: errno=%d", errno);
        netpacket_close_socket();
        return;
    }

    netpacket_last_hello_us = now_us;
}

static bool netpacket_send_to_addr(const gpsp_netpacket_wire_t *packet,
                                   size_t packet_size,
                                   const struct sockaddr_in *dst_addr,
                                   uint16_t client_id)
{
    ssize_t sent_len;

    sent_len = sendto(netpacket_socket_fd,
                      packet,
                      packet_size,
                      0,
                      (const struct sockaddr *)dst_addr,
                      sizeof(*dst_addr));
    if (sent_len < 0 || (size_t)sent_len != packet_size) {
        ESP_LOGW(TAG, "Netplay UDP send failed for client %u: errno=%d",
                 client_id,
                 errno);
        netpacket_close_socket();
        return false;
    }

    return true;
}

void netpacket_poll_receive(void)
{
    gpsp_netpacket_wire_t packet;
    struct sockaddr_in src_addr;
    socklen_t src_addr_len = sizeof(src_addr);
    int64_t now_us = esp_timer_get_time();

    if (!netpacket_runtime_ready()) {
        return;
    }

    netpacket_refresh_client_count(now_us);

    if (netpacket_open_socket() != ESP_OK) {
        return;
    }

    netpacket_maybe_send_hello(now_us);

    while (1) {
        ssize_t recv_len;
        uint64_t src_node_uid;
        uint16_t src_client_id;
        uint16_t dst_client_id;
        size_t payload_len;

        memset(&src_addr, 0, sizeof(src_addr));
        src_addr_len = sizeof(src_addr);
        recv_len = recvfrom(netpacket_socket_fd,
                            &packet,
                            sizeof(packet),
                            0,
                            (struct sockaddr *)&src_addr,
                            &src_addr_len);
        if (recv_len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "Netplay UDP receive failed: errno=%d", errno);
                netpacket_close_socket();
            }
            break;
        }

        now_us = esp_timer_get_time();
        if (!netpacket_accept_wire_packet(&packet, (size_t)recv_len, &src_node_uid, &payload_len)) {
            continue;
        }

        netpacket_mark_peer_seen(src_node_uid,
                     &src_addr,
                     packet.packet_serial_mode,
                     packet.preferred_client_id,
                     now_us);
        netpacket_refresh_client_count(now_us);

        if (packet.packet_type != GPSP_NETPACKET_TYPE_DATA) {
            continue;
        }

        if (!netpacket_local_assigned ||
            !netpacket_find_assigned_client_id(src_node_uid, &src_client_id)) {
            continue;
        }

        dst_client_id = ntohs(packet.dst_client_id);
        if (dst_client_id != GPSP_NETPACKET_BROADCAST && dst_client_id != netplay_client_id) {
            continue;
        }

        netpacket_receive_dispatch(packet.payload, payload_len, src_client_id);
    }
}

void netpacket_send(uint16_t client_id, const void *buf, size_t len)
{
    int64_t now_us = esp_timer_get_time();
    gpsp_netpacket_wire_t packet = {
        .magic = htonl(GPSP_NETPACKET_MAGIC),
        .version = htons(GPSP_NETPACKET_WIRE_VERSION),
        .packet_type = GPSP_NETPACKET_TYPE_DATA,
        .packet_serial_mode = (uint8_t)serial_mode,
        .preferred_client_id = netpacket_local_preferred_client_id(),
        .reserved0 = 0,
        .dst_client_id = htons(client_id),
        .payload_len = htons((uint16_t)len),
    };
    size_t packet_size = offsetof(gpsp_netpacket_wire_t, payload) + len;

    if (!buf || len == 0) {
        return;
    }

    if (!netpacket_runtime_ready()) {
        return;
    }

    netpacket_refresh_client_count(now_us);
    netpacket_maybe_send_hello(now_us);

    if (!netpacket_local_assigned) {
        return;
    }

    if (len > GPSP_NETPACKET_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "Dropping oversized netpacket (%u bytes)", (unsigned)len);
        return;
    }

    if (netpacket_open_socket() != ESP_OK) {
        return;
    }

    netpacket_fill_node_uid(&packet, netpacket_local_uid());
    memcpy(packet.payload, buf, len);

    if (client_id == GPSP_NETPACKET_BROADCAST) {
        bool sent_any = false;

        for (uint16_t peer_client_id = 0; peer_client_id < netpacket_total_slots(); peer_client_id++) {
            netpacket_peer_state_t *peer;

            if (peer_client_id == netplay_client_id) {
                continue;
            }

            peer = netpacket_find_peer_for_client_id(peer_client_id);
            if (!peer) {
                continue;
            }

            sent_any = true;
            if (!netpacket_send_to_addr(&packet, packet_size, &peer->addr, peer_client_id)) {
                return;
            }
        }

        if (!sent_any) {
            return;
        }
    } else {
        netpacket_peer_state_t *peer = netpacket_find_peer_for_client_id(client_id);

        if (!peer || client_id == netplay_client_id) {
            return;
        }

        if (!netpacket_send_to_addr(&packet, packet_size, &peer->addr, client_id)) {
            return;
        }
    }
}
#else
void netpacket_poll_receive(void)
{
}

void netpacket_send(uint16_t client_id, const void *buf, size_t len)
{
    (void)client_id;
    (void)buf;
    (void)len;
}
#endif