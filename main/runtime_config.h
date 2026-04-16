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

extern bool gpsp_netplay_ra_enabled;
extern char gpsp_netplay_ra_host[64];
extern uint16_t gpsp_netplay_ra_port;
extern char gpsp_netplay_ra_nick[32];

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