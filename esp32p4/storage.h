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
#define STORAGE_STATE_DIR    STORAGE_MOUNT_POINT "/states"

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
 * Run a one-shot storage throughput benchmark for a ROM file path.
 * Measures stdio fread and raw sdmmc sector reads for diagnostic purposes.
 */
esp_err_t storage_run_benchmark(const char *rom_path);

/**
 * Save battery-backed RAM to a file.
 */
esp_err_t storage_write_save(const char *rom_name, const void *data, size_t size);

/**
 * Load battery-backed RAM from a file.
 */
esp_err_t storage_read_save(const char *rom_name, void *data, size_t buffer_size,
							size_t *bytes_read);

/**
 * Save emulator state blob to a file.
 */
esp_err_t storage_write_state(const char *rom_name, unsigned slot,
							  const void *data, size_t size);

/**
 * Load emulator state blob from a file.
 */
esp_err_t storage_read_state(const char *rom_name, unsigned slot,
							 void *data, size_t buffer_size, size_t *bytes_read);

/**
 * List ROM files in the storage directory.
 */
esp_err_t storage_list_roms(const char *dir_path, char ***entries, size_t *count);

/**
 * Free ROM list returned by storage_list_roms.
 */
void storage_free_rom_list(char **entries, size_t count);

/**
 * List existing state file slots for a ROM.
 * Scans slots 0..max_slots-1.  Writes found slot numbers into slots[],
 * their file sizes into slot_sizes[] (if non-NULL), and total found into *count.
 */
esp_err_t storage_list_states(const char *rom_name, unsigned *slots, size_t *slot_sizes,
                              size_t max_slots, size_t *count);

/**
 * Delete a state file for a ROM at the given slot.
 */
esp_err_t storage_delete_state(const char *rom_name, unsigned slot);

/**
 * Build the filesystem path for a state file.
 * Returns ESP_OK if the path was written to @p path_buf.
 */
esp_err_t storage_get_state_path(const char *rom_name, unsigned slot,
                                char *path_buf, size_t path_buf_size);

/**
 * Read the recent game list from storage.
 * Returns up to max_entries paths (full paths).  Caller must free each entry and the array.
 */
esp_err_t storage_read_recent_list(char ***entries, size_t *count, size_t max_entries);

/**
 * Update the recent game list: push rom_path to the front, removing duplicates.
 * Keeps at most max_entries.
 */
esp_err_t storage_update_recent_list(const char *rom_path, size_t max_entries);

/**
 * Unmount and deinitialize storage.
 */
void storage_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP32P4_STORAGE_H */
