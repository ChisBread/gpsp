/*
 * gpsp app support — ESP32-C6 remote link bring-up
 */

#ifndef GPSP_MAIN_C6_REMOTE_H
#define GPSP_MAIN_C6_REMOTE_H

#include "freertos/FreeRTOS.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t c6_remote_start_task(BaseType_t core_id, UBaseType_t priority);
bool c6_remote_network_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* GPSP_MAIN_C6_REMOTE_H */