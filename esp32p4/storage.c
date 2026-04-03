/*
 * gpsp ESP32-P4 Platform — Storage driver
 * SD card via SDMMC Slot 0 (IO MUX) + on-chip LDO power
 * Target board: JC4880P443C_I_W
 *
 * The JC4880 uses SDMMC Host Slot 0 which is wired through IO MUX,
 * so no explicit GPIO pin specification is needed. SD card power
 * comes from the ESP32-P4 on-chip LDO channel 4.
 */

#include "storage.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static const char *TAG = "gpsp_storage";

/* LDO channel for SD card power */
#define SD_PWR_LDO_CHANNEL  4

static struct {
    sdmmc_card_t *card;
    sd_pwr_ctrl_handle_t pwr_ctrl;
    bool initialized;
} s_storage;

static void build_content_path(char *path, size_t path_size, const char *dir_path,
                               const char *rom_name, const char *extension,
                               int slot)
{
    const char *base_name = rom_name;
    const char *dot_extension;
    size_t stem_len;

    if (!path || path_size == 0) {
        return;
    }

    if (!rom_name || rom_name[0] == '\0') {
        path[0] = '\0';
        return;
    }

    base_name = strrchr(rom_name, '/');
    base_name = base_name ? base_name + 1 : rom_name;

    dot_extension = strrchr(base_name, '.');
    if (!dot_extension || dot_extension == base_name) {
        dot_extension = base_name + strlen(base_name);
    }

    stem_len = (size_t)(dot_extension - base_name);
    if (stem_len > 200) {
        stem_len = 200;
    }

    if (slot >= 0) {
        snprintf(path, path_size, "%s/%.*s.slot%u.%s",
                 dir_path, (int)stem_len, base_name, (unsigned)slot, extension ? extension : "bin");
    } else {
        snprintf(path, path_size, "%s/%.*s.%s",
                 dir_path, (int)stem_len, base_name, extension ? extension : "bin");
    }
}

static void build_save_path(char *path, size_t path_size, const char *rom_name)
{
    build_content_path(path, path_size, STORAGE_SAVE_DIR, rom_name, "sav", -1);
}

static void build_state_path(char *path, size_t path_size, const char *rom_name, unsigned slot)
{
    build_content_path(path, path_size, STORAGE_STATE_DIR, rom_name, "state", (int)slot);
}

esp_err_t storage_init(void)
{
    if (s_storage.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- SD card power via on-chip LDO ---- */
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = SD_PWR_LDO_CHANNEL,
    };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_storage.pwr_ctrl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init SD card LDO power: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- Mount configuration ---- */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 64 * 1024,
    };

    /* ---- SDMMC host: Slot 0, high speed ---- */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = s_storage.pwr_ctrl;

    /* Slot 0 uses IO MUX — no need to specify GPIO pins */
    sdmmc_slot_config_t slot_config = {
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };

    err = esp_vfs_fat_sdmmc_mount(
        STORAGE_MOUNT_POINT, &host, &slot_config, &mount_config, &s_storage.card
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_card_print_info(stdout, s_storage.card);

    /* Create saves directory if it doesn't exist */
    struct stat st;
    if (stat(STORAGE_SAVE_DIR, &st) != 0) {
        mkdir(STORAGE_SAVE_DIR, 0755);
    }
    if (stat(STORAGE_STATE_DIR, &st) != 0) {
        mkdir(STORAGE_STATE_DIR, 0755);
    }

    s_storage.initialized = true;
    ESP_LOGI(TAG, "SD card mounted at %s (4-bit, %d kHz)",
             STORAGE_MOUNT_POINT, SDMMC_FREQ_HIGHSPEED);
    return ESP_OK;
}

esp_err_t storage_write_save(const char *rom_name, const void *data, size_t size)
{
    char path[256];
    build_save_path(path, sizeof(path), rom_name);

    if (path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot create save file: %s", path);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, size, f);
    fclose(f);

    if (written != size) {
        ESP_LOGE(TAG, "Save write incomplete: %u / %u", (unsigned)written, (unsigned)size);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Save written: %s (%u bytes)", path, (unsigned)size);
    return ESP_OK;
}

esp_err_t storage_read_save(const char *rom_name, void *data, size_t buffer_size,
                            size_t *bytes_read)
{
    char path[256];
    size_t read = 0;

    build_save_path(path, sizeof(path), rom_name);

    if (path[0] == '\0' || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    read = fread(data, 1, buffer_size, f);
    fclose(f);

    if (bytes_read) {
        *bytes_read = read;
    }

    ESP_LOGI(TAG, "Save loaded: %s (%u bytes)", path, (unsigned)read);
    return ESP_OK;
}

esp_err_t storage_write_state(const char *rom_name, unsigned slot,
                              const void *data, size_t size)
{
    char path[256];
    FILE *f;
    size_t written;

    build_state_path(path, sizeof(path), rom_name, slot);

    if (path[0] == '\0' || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot create state file: %s", path);
        return ESP_FAIL;
    }

    written = fwrite(data, 1, size, f);
    fclose(f);

    if (written != size) {
        ESP_LOGE(TAG, "State write incomplete: %u / %u", (unsigned)written, (unsigned)size);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "State written: %s (%u bytes)", path, (unsigned)size);
    return ESP_OK;
}

esp_err_t storage_read_state(const char *rom_name, unsigned slot,
                             void *data, size_t buffer_size, size_t *bytes_read)
{
    char path[256];
    FILE *f;
    size_t read;

    build_state_path(path, sizeof(path), rom_name, slot);

    if (path[0] == '\0' || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    read = fread(data, 1, buffer_size, f);
    fclose(f);

    if (bytes_read) {
        *bytes_read = read;
    }

    ESP_LOGI(TAG, "State loaded: %s (%u bytes)", path, (unsigned)read);
    return ESP_OK;
}

esp_err_t storage_list_roms(const char *dir_path, char ***entries, size_t *count)
{
    const char *scan_path = dir_path ? dir_path : STORAGE_MOUNT_POINT;

    DIR *dir = opendir(scan_path);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open directory: %s", scan_path);
        return ESP_ERR_NOT_FOUND;
    }

    /* First pass: count .gba files */
    size_t num = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcasecmp(&entry->d_name[len - 4], ".gba") == 0) {
            num++;
        }
    }

    if (num == 0) {
        closedir(dir);
        *entries = NULL;
        *count = 0;
        return ESP_OK;
    }

    /* Allocate array */
    char **list = calloc(num, sizeof(char *));
    if (!list) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    /* Second pass: collect names */
    rewinddir(dir);
    size_t idx = 0;
    while ((entry = readdir(dir)) != NULL && idx < num) {
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcasecmp(&entry->d_name[len - 4], ".gba") == 0) {
            list[idx] = strdup(entry->d_name);
            idx++;
        }
    }
    closedir(dir);

    *entries = list;
    *count = idx;
    ESP_LOGI(TAG, "Found %u ROM files in %s", (unsigned)idx, scan_path);
    return ESP_OK;
}

void storage_free_rom_list(char **entries, size_t count)
{
    if (entries) {
        for (size_t i = 0; i < count; i++) {
            free(entries[i]);
        }
        free(entries);
    }
}

void storage_deinit(void)
{
    if (s_storage.card) {
        esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT_POINT, s_storage.card);
        s_storage.card = NULL;
    }
    /* Note: LDO power handle is typically not freed */
    s_storage.initialized = false;
    ESP_LOGI(TAG, "Storage deinitialized");
}
