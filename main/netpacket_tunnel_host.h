#ifndef GPSP_MAIN_NETPACKET_TUNNEL_HOST_H
#define GPSP_MAIN_NETPACKET_TUNNEL_HOST_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void netpacket_tunnel_host_poll(void);
const char *netpacket_tunnel_host_session_id(void);    /* tunnel session ID (24 hex chars) */
const char *netpacket_tunnel_host_room_id(void);       /* numeric lobby room ID string */
const char *netpacket_tunnel_host_status(void);
bool netpacket_tunnel_host_room_ready(void);

#ifdef __cplusplus
}
#endif

#endif