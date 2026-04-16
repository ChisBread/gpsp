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

extern int  gpsp_netplay_ra_mode;
extern bool gpsp_netplay_ra_enabled;   /* derived: mode != 0 */
extern char gpsp_netplay_ra_host[64];
extern uint16_t gpsp_netplay_ra_port;
extern char gpsp_netplay_ra_nick[32];
extern char gpsp_netplay_ra_tunnel_id[25]; /* 24 hex chars + NUL */
extern char gpsp_netplay_lobby_host[64];
extern uint16_t gpsp_netplay_lobby_port;
extern char gpsp_netplay_lobby_relay[32];

extern int gpsp_serial_setting;
extern int gpsp_rtc_mode;

extern bool gpsp_web_server_enabled;

extern uint32_t gpsp_frameskip_type;
extern uint32_t gpsp_frameskip_interval;
extern uint32_t gpsp_frameskip_threshold;

esp_err_t gpsp_runtime_config_init(void);
esp_err_t gpsp_runtime_config_save(void);

#ifdef __cplusplus
}
#endif

#endif