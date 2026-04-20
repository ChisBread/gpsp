/*
 * Shared RetroArch Netplay protocol helpers.
 */

#include "ra_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "mbedtls/md.h"

#include "common.h"
#include "gpsp_config.h"
#include "serial.h"

/* ── Handshake helpers ────────────────────────────────────────────── */

uint32_t ra_platform_magic(void)
{
    /* Matches RetroArch's netplay_platform_magic():
     *   ((1 == htonl(1)) << 30) | (sizeof(size_t) << 15) | sizeof(long)
     * ESP32-P4 is little-endian, so bit 30 = 0.
     */
    return (0u << 30) | (sizeof(size_t) << 15) | sizeof(long);
}

uint32_t ra_impl_magic(void)
{
    const char *ver = GPSP_VERSION;
    uint32_t magic = 0;
    size_t i;

    for (i = 0; ver[i]; i++)
        magic ^= (uint32_t)ver[i] << (i & 0xf);
    magic ^= RA_NETPLAY_PROTOCOL_VERSION << (i & 0xf);
    return magic;
}

void ra_password_hash_hex(uint32_t salt, const char *password,
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

/* ── Socket helpers ───────────────────────────────────────────────── */

bool ra_resolve_host(const char *host, uint16_t port,
                     struct sockaddr_in *out_addr)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char port_str[8];

    if (!host || !host[0] || !out_addr)
        return false;

    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->sin_family = AF_INET;
    out_addr->sin_port = htons(port);

    if (inet_aton(host, &out_addr->sin_addr))
        return true;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return false;

    memcpy(out_addr, res->ai_addr, sizeof(*out_addr));
    freeaddrinfo(res);
    return true;
}

void ra_socket_prepare(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

bool ra_send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return false;
    }
    return true;
}

/* ── Per-serial-mode NETPACKET dispatch ──────────────────────────── */

void ra_receive_dispatch(const void *buf, size_t len, uint16_t client_id)
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
