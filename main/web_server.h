/*
 * gpsp ESP32-P4 — Embedded HTTP/WebSocket server
 * Provides web-based GBA debug input, and a foundation for
 * OTA updates, data transfer, and other HTTP endpoints.
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the HTTP + WebSocket server.
 * Call after WiFi/network is up.
 */
esp_err_t web_server_start(void);

/**
 * Start the server asynchronously — spawns a task pinned to the given core
 * that waits for the network stack to be ready before calling web_server_start().
 */
void web_server_start_async(int core_id);

/**
 * Stop the server and free resources.
 */
esp_err_t web_server_stop(void);

/**
 * Read the current button state from web clients.
 * Returns bitmask of GBA_KEY_* values (same as input_driver_read).
 * Thread-safe; can be called from the emulation loop.
 */
uint16_t web_server_input_read(void);

#ifdef __cplusplus
}
#endif

#endif /* WEB_SERVER_H */
