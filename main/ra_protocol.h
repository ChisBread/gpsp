#ifndef GPSP_MAIN_RA_PROTOCOL_H
#define GPSP_MAIN_RA_PROTOCOL_H

/*
 * Shared RetroArch Netplay protocol constants and helpers.
 *
 * Used by:
 *   netpacket_bridge_ra.c   (TCP client)
 *   netpacket_host.c        (TCP host)
 *   netpacket_tunnel_host.c (tunnel control path)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "lwip/sockets.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Protocol magic numbers and sizes ─────────────────────────────── */

#define RA_NETPLAY_MAGIC             0x52414E50u  /* "RANP" */
#define RA_NETPLAY_PROTOCOL_VERSION  7u
#define RA_NICK_LEN                  32
#define RA_PASS_HASH_LEN             64
#define RA_MAX_INPUT_DEVICES         16

/* ── Protocol command IDs ─────────────────────────────────────────── */

#define RA_CMD_DISCONNECT            0x000Au
#define RA_CMD_NICK                  0x0020u
#define RA_CMD_PASSWORD              0x0021u
#define RA_CMD_INFO                  0x0022u
#define RA_CMD_SYNC                  0x0023u
#define RA_CMD_SPECTATE              0x0024u
#define RA_CMD_PLAY                  0x0025u
#define RA_CMD_MODE                  0x0026u
#define RA_CMD_MODE_REFUSED          0x0027u
#define RA_CMD_NETPACKET             0x0048u
#define RA_CMD_PING_REQUEST          0x1100u
#define RA_CMD_PING_RESPONSE         0x1101u
#define RA_CMD_SETTING_ALLOW_PAUSE   0x2000u
#define RA_CMD_SETTING_INPUT_LATENCY 0x2001u

#define RA_SYNC_BIT_PAUSED           (1u << 31)

#define NETPACKET_BROADCAST          0xFFFFu
#define NETPACKET_MAX_PAYLOAD        2048u
#define NETPACKET_SEND_QUEUE_CAPACITY (16 * 1024u)

/* ── Handshake helpers ────────────────────────────────────────────── */

/* RetroArch netplay_platform_magic() equivalent. */
uint32_t ra_platform_magic(void);

/* RetroArch netplay_impl_magic() equivalent based on GPSP_VERSION. */
uint32_t ra_impl_magic(void);

/* Compute "%08X<password>" SHA256 as lowercase hex into out_hex
 * (must be RA_PASS_HASH_LEN+1 bytes).  Safe with password=NULL. */
void ra_password_hash_hex(uint32_t salt, const char *password,
                          char out_hex[RA_PASS_HASH_LEN + 1]);

/* ── Socket helpers ───────────────────────────────────────────────── */

/* Resolve a host[:port] to an IPv4 sockaddr.  Accepts either dotted-quad
 * literals or DNS names.  Returns false on failure. */
bool ra_resolve_host(const char *host, uint16_t port,
                     struct sockaddr_in *out_addr);

/* Put fd in non-blocking mode and enable TCP_NODELAY.  Ignores failures
 * of TCP_NODELAY (best-effort). */
void ra_socket_prepare(int fd);

/* Blocking send loop — retries on EAGAIN/EWOULDBLOCK.  Returns true if
 * the full `len` bytes were sent. */
bool ra_send_all(int fd, const void *data, size_t len);

/* ── Ring buffer consume ──────────────────────────────────────────── */

/* Remove the first `n` bytes from a linear receive buffer, shifting the
 * remaining contents down.  If n >= *len, the buffer is fully cleared. */
static inline void ra_buf_consume(uint8_t *buf, size_t *len, size_t n)
{
    if (n >= *len) {
        *len = 0;
        return;
    }
    memmove(buf, buf + n, *len - n);
    *len -= n;
}

/* ── Per-serial-mode NETPACKET dispatch ──────────────────────────── */

/* Route a received NETPACKET payload to rfu/serialpoke/serialaw based on
 * the global serial_mode.  Unknown modes drop silently. */
void ra_receive_dispatch(const void *buf, size_t len, uint16_t client_id);

#ifdef __cplusplus
}
#endif

#endif /* GPSP_MAIN_RA_PROTOCOL_H */
