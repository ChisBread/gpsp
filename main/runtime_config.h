/*
 * gpsp app support — runtime configuration loaded from SD card
 */

#ifndef GPSP_MAIN_RUNTIME_CONFIG_H
#define GPSP_MAIN_RUNTIME_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GPSP_RUNTIME_CONFIG_PATH STORAGE_MOUNT_POINT "/gpsp.cfg"

/* Netplay mode: 0=disabled, 1=client (direct), 2=host, 3=tunnel client, 4=tunnel host */
#define NETPLAY_MODE_DISABLED       0
#define NETPLAY_MODE_CLIENT         1
#define NETPLAY_MODE_HOST           2
#define NETPLAY_MODE_TUNNEL_CLIENT  3
#define NETPLAY_MODE_TUNNEL_HOST    4

/* Netplay role (UI-level, mode is derived from role + use_tunnel) */
#define NETPLAY_ROLE_OFF     0
#define NETPLAY_ROLE_HOST    1
#define NETPLAY_ROLE_CLIENT  2

extern int  gpsp_netplay_ra_mode;      /* derived: from role + tunnel flag */
extern bool gpsp_netplay_ra_enabled;   /* derived: mode != 0 */
extern int  gpsp_netplay_role;          /* 0=off, 1=host, 2=client */
extern bool gpsp_netplay_use_tunnel;    /* use relay server */
extern bool gpsp_netplay_use_lobby;     /* publish to lobby */
extern char gpsp_netplay_ra_host[64];
extern uint16_t gpsp_netplay_ra_port;
extern char gpsp_netplay_ra_nick[32];
extern char gpsp_netplay_ra_tunnel_id[25]; /* 24 hex chars + NUL */
extern char gpsp_netplay_host_password[32];
extern char gpsp_netplay_client_password[32];
extern char gpsp_netplay_lobby_host[64];
extern uint16_t gpsp_netplay_lobby_port;
extern char gpsp_netplay_lobby_relay[32];
extern char gpsp_netplay_lobby_country[4];

/* Derive gpsp_netplay_ra_mode from role + tunnel flag */
void gpsp_netplay_update_mode(void);

extern int gpsp_serial_setting;
extern int gpsp_rtc_mode;

extern bool gpsp_web_server_enabled;

extern uint32_t gpsp_frameskip_type;
extern uint32_t gpsp_frameskip_interval;
extern uint32_t gpsp_frameskip_threshold;

/* Async ROM loading: when true, load_gamepak_raw streams only the
 * first 1 MB synchronously and the remainder is prefetched in the
 * background. When false, the synchronous loader is used. */
extern bool gpsp_rom_async_load;

esp_err_t gpsp_runtime_config_init(void);
esp_err_t gpsp_runtime_config_save(void);

#ifdef __cplusplus
}
#endif

#endif