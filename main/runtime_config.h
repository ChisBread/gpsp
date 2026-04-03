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

extern bool gpsp_netplay_udp_enabled;
extern uint16_t gpsp_netplay_udp_port;
extern uint32_t gpsp_netplay_peer_timeout_ms;
extern uint32_t gpsp_netplay_hello_interval_ms;
extern int gpsp_netplay_local_client_id_override;
extern char gpsp_netplay_broadcast_addr[16];

esp_err_t gpsp_runtime_config_init(void);
esp_err_t gpsp_runtime_config_save(void);

#ifdef __cplusplus
}
#endif

#endif