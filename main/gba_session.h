/*
 * gpsp app support — GBA session lifecycle and content reload control
 */

#ifndef GPSP_MAIN_GBA_SESSION_H
#define GPSP_MAIN_GBA_SESSION_H

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *rom_path;
    const char *bios_path;
} gba_session_boot_config_t;

typedef struct {
    const char *rom_path;
    const char *bios_path;
    bool reload_rom;
    bool reload_bios;
    bool clear_backup;
} gba_session_reload_request_t;

esp_err_t gba_session_init(const gba_session_boot_config_t *config);
esp_err_t gba_session_request_soft_reset(void);
esp_err_t gba_session_request_reload(const gba_session_reload_request_t *request);
esp_err_t gba_session_save_state(unsigned slot);
esp_err_t gba_session_load_state(unsigned slot);
esp_err_t gba_session_shutdown(void);
esp_err_t gba_session_process_pending(void);
void gba_emulation_task(void *param);

/**
 * Return a pointer to the current ROM path, or NULL if no content is loaded.
 * The returned pointer is valid until the next reload.
 */
const char *gba_session_current_rom_path(void);

/**
 * Write a JSON snapshot of current rolling-window performance stats into buf.
 * Returns the number of bytes written (excluding NUL), or -1 on error.
 */
int gba_session_stats_json(char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* GPSP_MAIN_GBA_SESSION_H */