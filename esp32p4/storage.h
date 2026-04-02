/*
 * gpsp ESP32-P4 Platform — Storage driver header
 * SD card via SDMMC Slot 0 (IO MUX) + on-chip LDO power
 * Target board: JC4880P443C_I_W
 */

#ifndef ESP32P4_STORAGE_H
#define ESP32P4_STORAGE_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Storage mount point */
#define STORAGE_MOUNT_POINT  "/sdcard"
#define STORAGE_BIOS_PATH    STORAGE_MOUNT_POINT "/gba_bios.bin"
#define STORAGE_SAVE_DIR     STORAGE_MOUNT_POINT "/saves"

/*
 * JC4880 SD card uses SDMMC Host Slot 0 with IO MUX pins:
 *   CLK=GPIO43, CMD=GPIO44, D0=GPIO39, D1=GPIO40, D2=GPIO41, D3=GPIO42
 * Power is provided via on-chip LDO channel 4.
 * No explicit GPIO configuration needed — slot 0 uses IO MUX.
 */

/**
 * Initialize SD card (SDMMC Slot 0, 4-bit, LDO power) and mount FAT filesystem.
 */
esp_err_t storage_init(void);

/**
 * Save battery-backed RAM to a file.
 */
esp_err_t storage_write_save(const char *rom_name, const void *data, size_t size);

/**
 * Load battery-backed RAM from a file.
 */
esp_err_t storage_read_save(const char *rom_name, void *data, size_t size);

/**
 * List ROM files in the storage directory.
 */
esp_err_t storage_list_roms(const char *dir_path, char ***entries, size_t *count);

/**
 * Free ROM list returned by storage_list_roms.
 */
void storage_free_rom_list(char **entries, size_t count);

/**
 * Unmount and deinitialize storage.
 */
void storage_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP32P4_STORAGE_H */
