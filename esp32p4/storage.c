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
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_idf_version.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "streams/file_stream.h"

/*
 * When ESP-Hosted uses SDIO on Slot 1, it initialises the SDMMC host
 * controller via a constructor before app_main().  The controller can
 * only be initialised once, so we replace host.init / host.deinit with
 * dummy functions so the SD-card mount on Slot 0 does not try to
 * re-initialise it.  Reference: esp_hosted example host_sdcard_with_hosted.
 */
#if defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE) && \
    (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
#define WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT 1
static esp_err_t sdmmc_host_init_dummy(void)   { return ESP_OK; }
static esp_err_t sdmmc_host_deinit_dummy(void)  { return ESP_OK; }
#else
#define WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT 0
#endif

static const char *TAG = "gpsp_storage";

/* LDO channel for SD card power */
#define SD_PWR_LDO_CHANNEL  4
#define STORAGE_BENCH_TOTAL_BYTES (1024 * 1024)
#define STORAGE_BENCH_CHUNK_BYTES (64 * 1024)

static struct {
    sdmmc_card_t *card;
    sd_pwr_ctrl_handle_t pwr_ctrl;
    bool initialized;
    bool benchmark_done;
} s_storage;

static uint32_t kib_per_sec(size_t bytes, int64_t us)
{
    if (us <= 0) {
        return 0;
    }
    return (uint32_t)((bytes * 1000000ULL) / 1024ULL / (uint64_t)us);
}

static void storage_log_memcpy_benchmark(const uint8_t *src, size_t size)
{
    uint8_t *psram_dst;
    size_t free_psram;
    size_t largest_psram;
    size_t bench_size;
    size_t copied;
    int64_t t0;
    int64_t t1;

    free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bench_size = size;

    if (largest_psram < bench_size) {
        bench_size = largest_psram;
    }
    if (bench_size > STORAGE_BENCH_CHUNK_BYTES) {
        bench_size = STORAGE_BENCH_CHUNK_BYTES;
    }

    ESP_LOGI(TAG,
             "Storage bench PSRAM free %u KB | largest %u KB | requested %u KB | test %u KB",
             (unsigned)(free_psram / 1024),
             (unsigned)(largest_psram / 1024),
             (unsigned)(size / 1024),
             (unsigned)(bench_size / 1024));

    if (bench_size == 0) {
        ESP_LOGW(TAG, "Storage bench memcpy skipped: no PSRAM block available");
        return;
    }

    psram_dst = heap_caps_malloc(bench_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!psram_dst) {
        ESP_LOGW(TAG, "Storage bench memcpy skipped: PSRAM alloc failed for %u KB",
                 (unsigned)(bench_size / 1024));
        return;
    }

    t0 = esp_timer_get_time();
    for (copied = 0; copied < size; copied += bench_size) {
        size_t chunk = size - copied;
        if (chunk > bench_size) {
            chunk = bench_size;
        }
        memcpy(psram_dst, src, chunk);
    }
    t1 = esp_timer_get_time();

    ESP_LOGI(TAG,
             "Storage bench memcpy->PSRAM: %u KB in %lld us | %u KiB/s",
             (unsigned)(size / 1024),
             (long long)(t1 - t0),
             (unsigned)kib_per_sec(size, t1 - t0));

    heap_caps_free(psram_dst);
}

static void storage_log_filestream_psram_benchmark(const char *rom_path, size_t size)
{
    RFILE *file;
    uint8_t *psram_dst;
    int64_t t0;
    int64_t t1;
    int64_t got;

    if (!rom_path || rom_path[0] == '\0' || size == 0) {
        return;
    }

    psram_dst = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!psram_dst) {
        ESP_LOGW(TAG, "Storage bench filestream->PSRAM skipped: no PSRAM buffer");
        return;
    }

    file = filestream_open(rom_path, RETRO_VFS_FILE_ACCESS_READ,
                           RETRO_VFS_FILE_ACCESS_HINT_NONE);
    if (!file) {
        ESP_LOGW(TAG, "Storage bench filestream->PSRAM skipped: cannot open %s", rom_path);
        heap_caps_free(psram_dst);
        return;
    }

    t0 = esp_timer_get_time();
    got = filestream_read(file, psram_dst, (int64_t)size);
    t1 = esp_timer_get_time();

    if (filestream_close(file) != 0) {
        ESP_LOGW(TAG, "Storage bench filestream->PSRAM close failed");
    }

    if (got < 0) {
        ESP_LOGW(TAG, "Storage bench filestream->PSRAM read failed");
    } else {
        ESP_LOGI(TAG,
                 "Storage bench filestream->PSRAM: %u KB in %lld us | %u KiB/s",
                 (unsigned)(got / 1024),
                 (long long)(t1 - t0),
                 (unsigned)kib_per_sec((size_t)got, t1 - t0));
    }

    heap_caps_free(psram_dst);
}

static void storage_log_fread_psram_benchmark(const char *rom_path, size_t size)
{
    FILE *file;
    uint8_t *psram_dst;
    int64_t t0;
    int64_t t1;
    size_t got;

    if (!rom_path || rom_path[0] == '\0' || size == 0) {
        return;
    }

    psram_dst = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!psram_dst) {
        ESP_LOGW(TAG, "Storage bench fread->PSRAM skipped: no PSRAM buffer");
        return;
    }

    file = fopen(rom_path, "rb");
    if (!file) {
        ESP_LOGW(TAG, "Storage bench fread->PSRAM skipped: cannot open %s", rom_path);
        heap_caps_free(psram_dst);
        return;
    }

    t0 = esp_timer_get_time();
    got = fread(psram_dst, 1, size, file);
    t1 = esp_timer_get_time();

    fclose(file);

    ESP_LOGI(TAG,
             "Storage bench fread->PSRAM: %u KB in %lld us | %u KiB/s",
             (unsigned)(got / 1024),
             (long long)(t1 - t0),
             (unsigned)kib_per_sec(got, t1 - t0));

    heap_caps_free(psram_dst);
}

static void storage_log_raw_psram_benchmark(size_t size)
{
    uint8_t *psram_dst;
    int64_t t0;
    int64_t t1;
    size_t sector_size;
    size_t total_sectors;
    size_t start_sector = 4096;
    esp_err_t err;

    if (!s_storage.card || size == 0) {
        return;
    }

    sector_size = (size_t)s_storage.card->csd.sector_size;
    if (sector_size == 0 || (size % sector_size) != 0) {
        ESP_LOGW(TAG, "Storage bench raw->PSRAM skipped: invalid size %u", (unsigned)size);
        return;
    }

    total_sectors = size / sector_size;
    if ((uint64_t)start_sector + total_sectors > (uint64_t)s_storage.card->csd.capacity) {
        start_sector = 0;
    }

    psram_dst = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!psram_dst) {
        ESP_LOGW(TAG, "Storage bench raw->PSRAM skipped: no PSRAM buffer");
        return;
    }

    t0 = esp_timer_get_time();
    err = sdmmc_read_sectors(s_storage.card, psram_dst, start_sector, total_sectors);
    t1 = esp_timer_get_time();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Storage bench raw->PSRAM failed: %s", esp_err_to_name(err));
        heap_caps_free(psram_dst);
        return;
    }

    ESP_LOGI(TAG,
             "Storage bench raw->PSRAM: %u KB in %lld us | %u KiB/s | start_sector %u",
             (unsigned)(size / 1024),
             (long long)(t1 - t0),
             (unsigned)kib_per_sec(size, t1 - t0),
             (unsigned)start_sector);

    heap_caps_free(psram_dst);
}

esp_err_t storage_run_benchmark(const char *rom_path)
{
    uint8_t *buffer;
    size_t bench_total = STORAGE_BENCH_TOTAL_BYTES;
    size_t chunk_size = STORAGE_BENCH_CHUNK_BYTES;
    size_t file_total = 0;
    size_t raw_total = 0;
    FILE *file = NULL;
    int64_t t0;
    int64_t t1;

    if (s_storage.benchmark_done) {
        return ESP_OK;
    }

    if (!s_storage.initialized || !s_storage.card || !rom_path || rom_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    buffer = heap_caps_malloc(chunk_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!buffer) {
        ESP_LOGW(TAG, "Storage benchmark skipped: no internal DMA buffer");
        s_storage.benchmark_done = true;
        return ESP_ERR_NO_MEM;
    }

    file = fopen(rom_path, "rb");
    if (file) {
        t0 = esp_timer_get_time();
        while (file_total < bench_total) {
            size_t to_read = bench_total - file_total;
            if (to_read > chunk_size) {
                to_read = chunk_size;
            }

            size_t got = fread(buffer, 1, to_read, file);
            if (got == 0) {
                break;
            }
            file_total += got;
            if (got < to_read) {
                break;
            }
        }
        t1 = esp_timer_get_time();
        fclose(file);
        ESP_LOGI(TAG,
                 "Storage bench fread: %u KB in %lld us | %u KiB/s",
                 (unsigned)(file_total / 1024),
                 (long long)(t1 - t0),
                 (unsigned)kib_per_sec(file_total, t1 - t0));
    } else {
        ESP_LOGW(TAG, "Storage bench fread skipped: cannot open %s", rom_path);
    }

    {
        size_t sector_size = (size_t)s_storage.card->csd.sector_size;
        size_t total_sectors = bench_total / sector_size;
        size_t chunk_sectors = chunk_size / sector_size;
        size_t start_sector = 4096;

        if (sector_size == 0 || total_sectors == 0 || chunk_sectors == 0) {
            ESP_LOGW(TAG, "Storage bench raw skipped: invalid sector geometry");
        } else {
            if ((uint64_t)start_sector + total_sectors > (uint64_t)s_storage.card->csd.capacity) {
                start_sector = 0;
            }

            t0 = esp_timer_get_time();
            for (size_t done = 0; done < total_sectors; done += chunk_sectors) {
                size_t sectors = total_sectors - done;
                if (sectors > chunk_sectors) {
                    sectors = chunk_sectors;
                }

                esp_err_t err = sdmmc_read_sectors(
                    s_storage.card,
                    buffer,
                    start_sector + done,
                    sectors);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Storage bench raw failed at sector %u: %s",
                             (unsigned)(start_sector + done), esp_err_to_name(err));
                    break;
                }
                raw_total += sectors * sector_size;
            }
            t1 = esp_timer_get_time();
            ESP_LOGI(TAG,
                     "Storage bench raw: %u KB in %lld us | %u KiB/s | start_sector %u",
                     (unsigned)(raw_total / 1024),
                     (long long)(t1 - t0),
                     (unsigned)kib_per_sec(raw_total, t1 - t0),
                     (unsigned)start_sector);
        }
    }

    if (file_total > 0) {
        storage_log_memcpy_benchmark(buffer, file_total);
        storage_log_fread_psram_benchmark(rom_path, file_total);
        storage_log_raw_psram_benchmark(file_total);
        storage_log_filestream_psram_benchmark(rom_path, file_total);
    }

    heap_caps_free(buffer);
    s_storage.benchmark_done = true;
    return ESP_OK;
}

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
#if WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT
    /* Controller already initialised by ESP-Hosted for Slot 1 SDIO */
    host.init = &sdmmc_host_init_dummy;
    host.deinit = &sdmmc_host_deinit_dummy;
#endif

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
    ESP_LOGI(TAG,
             "SD negotiated: host_req %d kHz host_real %d kHz card_max %u kHz | bus %u-bit | DDR %s UHS-I %s | CSD tr_speed %d",
             host.max_freq_khz,
             s_storage.card->real_freq_khz,
             (unsigned)s_storage.card->max_freq_khz,
             (unsigned)(1U << s_storage.card->log_bus_width),
             s_storage.card->is_ddr ? "yes" : "no",
             s_storage.card->is_uhs1 ? "yes" : "no",
             s_storage.card->csd.tr_speed);

    /* Create saves directory if it doesn't exist */
    struct stat st;
    if (stat(STORAGE_SAVE_DIR, &st) != 0) {
        mkdir(STORAGE_SAVE_DIR, 0755);
    }
    if (stat(STORAGE_STATE_DIR, &st) != 0) {
        mkdir(STORAGE_STATE_DIR, 0755);
    }

    s_storage.initialized = true;
    s_storage.benchmark_done = false;
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

esp_err_t storage_get_state_path(const char *rom_name, unsigned slot,
                                char *path_buf, size_t path_buf_size)
{
    if (!rom_name || !path_buf || path_buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    build_state_path(path_buf, path_buf_size, rom_name, slot);
    if (path_buf[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
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

esp_err_t storage_list_states(const char *rom_name, unsigned *slots, size_t *slot_sizes,
                              size_t max_slots, size_t *count)
{
    char path[256];
    struct stat st;
    size_t found = 0;

    if (!slots || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    for (unsigned s = 0; s < max_slots; s++) {
        build_state_path(path, sizeof(path), rom_name, s);
        if (path[0] != '\0' && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            slots[found] = s;
            if (slot_sizes) {
                slot_sizes[found] = (size_t)st.st_size;
            }
            found++;
        }
    }

    *count = found;
    return ESP_OK;
}

esp_err_t storage_delete_state(const char *rom_name, unsigned slot)
{
    char path[256];
    build_state_path(path, sizeof(path), rom_name, slot);

    if (path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (remove(path) != 0) {
        ESP_LOGW(TAG, "Cannot delete state file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "State deleted: %s", path);
    return ESP_OK;
}

#define STORAGE_RECENT_PATH  STORAGE_MOUNT_POINT "/recent.txt"
#define STORAGE_RECENT_MAX_LINE 512

esp_err_t storage_read_recent_list(char ***entries, size_t *count, size_t max_entries)
{
    FILE *f;
    char line[STORAGE_RECENT_MAX_LINE];
    char **list;
    size_t num = 0;

    if (!entries || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    *entries = NULL;
    *count = 0;

    f = fopen(STORAGE_RECENT_PATH, "r");
    if (!f) {
        return ESP_OK;  /* no recent list yet — not an error */
    }

    list = calloc(max_entries, sizeof(char *));
    if (!list) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    while (num < max_entries && fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        list[num] = strdup(line);
        if (!list[num]) break;
        num++;
    }
    fclose(f);

    *entries = list;
    *count = num;
    return ESP_OK;
}

esp_err_t storage_update_recent_list(const char *rom_path, size_t max_entries)
{
    char **old_list = NULL;
    size_t old_count = 0;
    FILE *f;

    if (!rom_path || rom_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Read existing list */
    storage_read_recent_list(&old_list, &old_count, max_entries + 1);

    f = fopen(STORAGE_RECENT_PATH, "w");
    if (!f) {
        ESP_LOGW(TAG, "Cannot write recent list");
        if (old_list) {
            for (size_t i = 0; i < old_count; i++) free(old_list[i]);
            free(old_list);
        }
        return ESP_FAIL;
    }

    /* Write new entry first */
    fprintf(f, "%s\n", rom_path);

    /* Write old entries, skipping duplicate of new entry, up to max */
    size_t written = 1;
    for (size_t i = 0; i < old_count && written < max_entries; i++) {
        if (old_list[i] && strcmp(old_list[i], rom_path) != 0) {
            fprintf(f, "%s\n", old_list[i]);
            written++;
        }
    }

    fclose(f);

    if (old_list) {
        for (size_t i = 0; i < old_count; i++) free(old_list[i]);
        free(old_list);
    }

    ESP_LOGI(TAG, "Recent list updated: %s (%u entries)", rom_path, (unsigned)written);
    return ESP_OK;
}

void storage_deinit(void)
{
    if (s_storage.card) {
        esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT_POINT, s_storage.card);
        s_storage.card = NULL;
    }
    /* Note: LDO power handle is typically not freed */
    s_storage.initialized = false;
    s_storage.benchmark_done = false;
    ESP_LOGI(TAG, "Storage deinitialized");
}
