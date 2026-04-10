/*
 * gpsp app support — GBA session lifecycle and content reload control
 */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "av_pipeline.h"
#include "common.h"
#include "cpu.h"
#include "cpu_instrument.h"
#include "gba_memory.h"
#include "gba_session.h"
#include "input_driver.h"
#include "main.h"
#include "savestate.h"
#include "sound.h"
#include "storage.h"
#include "video.h"
#include "web_server.h"
#include "runtime_config.h"

#define GBA_SESSION_PATH_MAX 512
#define GBA_SESSION_QUEUE_LEN 4
#define GBA_SESSION_AUTOSAVE_PERIOD_US (5 * 1000 * 1000)
#define GBA_SESSION_STATE_MAGIC 0x53545347u
#define GBA_SESSION_STATE_VERSION 1u
#define GBA_SESSION_STATS_WINDOW 240
#define GBA_SESSION_STATE_IO_BUF_SIZE \
    (sizeof(gba_session_state_file_header_t) + GBA_STATE_MEM_SIZE + sizeof(gamepak_backup))

typedef enum {
    GBA_SESSION_CMD_SOFT_RESET = 0,
    GBA_SESSION_CMD_RELOAD,
    GBA_SESSION_CMD_SAVE_STATE,
    GBA_SESSION_CMD_LOAD_STATE,
    GBA_SESSION_CMD_SHUTDOWN,
} gba_session_command_type_t;

typedef struct {
    gba_session_command_type_t type;
    bool reload_rom;
    bool reload_bios;
    bool clear_backup;
    unsigned state_slot;
    char rom_path[GBA_SESSION_PATH_MAX];
    char bios_path[GBA_SESSION_PATH_MAX];
    TaskHandle_t completion_task;
    esp_err_t *result_ptr;
} gba_session_command_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t state_size;
    uint32_t backup_size;
} gba_session_state_file_header_t;

typedef struct {
    bool initialized;
    bool has_content;
    bool builtin_bios_active;
    bool stop_requested;
    char rom_path[GBA_SESSION_PATH_MAX];
    char bios_path[GBA_SESSION_PATH_MAX];
    QueueHandle_t control_queue;
    TaskHandle_t emulation_task;
    int64_t last_autosave_us;
    int64_t backup_dirty_since_us;  /* 0 = no pending dirty */
} gba_session_state_t;

typedef struct {
    int64_t samples[GBA_SESSION_STATS_WINDOW];
    size_t next_index;
    size_t count;
} frame_stat_window_t;

typedef struct {
    int64_t avg_us;
    int64_t p99_us;
    size_t sample_count;
} frame_stat_summary_t;

typedef struct {
    frame_stat_window_t cpu_us;
    frame_stat_window_t render_us;
    frame_stat_window_t submit_us;
    frame_stat_window_t acquire_us;
} gba_session_perf_stats_t;

#ifdef CPU_PROFILE_STATS
typedef struct {
    u32 frames;
    u32 total;
    /* exec = total - update */
    u32 exec;
    /* update_gba sub-components */
    u32 update;
    u32 scanline;
    u32 scanline_bg;
    u32 scanline_bg_text_fast;
    u32 scanline_bg_text_mosaic;
    u32 scanline_bg_affine;
    u32 scanline_bg_bitmap;
    u32 scanline_obj;
    u32 scanline_fx;
    u32 scanline_order;
    u32 scanline_affine;
    u32 scanline_blank;
    u32 sound;
    u32 dma;
    u32 timer;
    u32 serial;
    u32 irq;
    /* dynarec overhead (sub-components of exec) */
    u32 dynarec_lookup;
    u32 dynarec_translate;
    u32 dynarec_icache_sync;
    /* counts */
    u32 arm_count;
    u32 thumb_count;
    u32 dynarec_lookup_hits;
    u32 dynarec_lookup_misses;
} cpu_prof_snapshot_t;

static GPSP_EXTRAM_BSS cpu_prof_snapshot_t s_prof_snap;
#endif

static const char *TAG = "gpsp_session";
static GPSP_EXTRAM_BSS gba_session_state_t s_session;
static int64_t s_frame_start_us;
static uint32_t s_fps_counter;
static uint32_t s_fps_last_x10;   /* FPS × 10, e.g. 597 = 59.7 */
static int64_t s_fps_timer_us;
static GPSP_EXTRAM_BSS gba_session_perf_stats_t s_perf_stats;

static int compare_int64_ascending(const void *lhs, const void *rhs)
{
    const int64_t left = *(const int64_t *)lhs;
    const int64_t right = *(const int64_t *)rhs;

    if (left < right) {
        return -1;
    }
    if (left > right) {
        return 1;
    }
    return 0;
}

static void frame_stat_window_push(frame_stat_window_t *window, int64_t value_us)
{
    if (!window) {
        return;
    }

    window->samples[window->next_index] = value_us;
    window->next_index = (window->next_index + 1) % GBA_SESSION_STATS_WINDOW;
    if (window->count < GBA_SESSION_STATS_WINDOW) {
        window->count++;
    }
}

static frame_stat_summary_t frame_stat_window_summarize(const frame_stat_window_t *window)
{
    frame_stat_summary_t summary = {0};

    if (!window || window->count == 0) {
        return summary;
    }

    int64_t ordered[GBA_SESSION_STATS_WINDOW];
    int64_t total_us = 0;

    for (size_t i = 0; i < window->count; i++) {
        ordered[i] = window->samples[i];
        total_us += ordered[i];
    }

    qsort(ordered, window->count, sizeof(ordered[0]), compare_int64_ascending);

    summary.avg_us = total_us / (int64_t)window->count;
    summary.p99_us = ordered[((window->count * 99) + 99) / 100 - 1];
    summary.sample_count = window->count;
    return summary;
}

static void format_frame_stat_summary(char *buffer, size_t buffer_size,
                                      const frame_stat_summary_t *summary)
{
    if (!buffer || buffer_size == 0) {
        return;
    }

    if (!summary || summary->sample_count == 0) {
        snprintf(buffer, buffer_size, "n/a");
        return;
    }

    snprintf(buffer, buffer_size,
             "avg %lld top99 %lld us",
             (long long)summary->avg_us,
             (long long)summary->p99_us);
}

static void gba_session_perf_reset(void)
{
    memset(&s_perf_stats, 0, sizeof(s_perf_stats));
}

static void gba_session_perf_push_single_core(int64_t cpu_us,
                                              int64_t render_us,
                                              int64_t submit_us,
                                              int64_t acquire_us)
{
    frame_stat_window_push(&s_perf_stats.cpu_us, cpu_us);
    frame_stat_window_push(&s_perf_stats.render_us, render_us);
    frame_stat_window_push(&s_perf_stats.submit_us, submit_us);
    frame_stat_window_push(&s_perf_stats.acquire_us, acquire_us);
}

static void gba_session_perf_log_single_core(void)
{
    char cpu_buf[48];
    char render_buf[48];
    char submit_buf[48];
    char acquire_buf[48];
    frame_stat_summary_t cpu = frame_stat_window_summarize(&s_perf_stats.cpu_us);
    frame_stat_summary_t render = frame_stat_window_summarize(&s_perf_stats.render_us);
    frame_stat_summary_t submit = frame_stat_window_summarize(&s_perf_stats.submit_us);
    frame_stat_summary_t acquire = frame_stat_window_summarize(&s_perf_stats.acquire_us);

    format_frame_stat_summary(cpu_buf, sizeof(cpu_buf), &cpu);
    format_frame_stat_summary(render_buf, sizeof(render_buf), &render);
    format_frame_stat_summary(submit_buf, sizeof(submit_buf), &submit);
    format_frame_stat_summary(acquire_buf, sizeof(acquire_buf), &acquire);

    ESP_LOGI(TAG,
             "Frame CPU (%u samples): run [%s] | render [%s]",
             (unsigned)cpu.sample_count,
             cpu_buf,
             render_buf);
    ESP_LOGI(TAG,
             "Frame IO  (%u samples): submit [%s] | acquire [%s] | heap %u KB",
             (unsigned)submit.sample_count,
             submit_buf,
             acquire_buf,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
}

#ifdef CPU_PROFILE_STATS
static void gba_session_snapshot_cpu_prof(void)
{
    u32 f, update;

    if (cpu_prof.frames == 0) {
        return;
    }

    f = cpu_prof.frames;
    update = cpu_prof.update_cycles / f;

    s_prof_snap.frames = f;
    s_prof_snap.total = cpu_prof.total_cycles / f;
    s_prof_snap.update = update;
    s_prof_snap.exec = (s_prof_snap.total > update)
                     ? (s_prof_snap.total - update) : 0;
    /* update_gba sub-components */
    s_prof_snap.scanline = cpu_prof.scanline_cycles / f;
    s_prof_snap.scanline_bg = cpu_prof.scanline_bg_cycles / f;
    s_prof_snap.scanline_bg_text_fast = cpu_prof.scanline_bg_text_fast_cycles / f;
    s_prof_snap.scanline_bg_text_mosaic = cpu_prof.scanline_bg_text_mosaic_cycles / f;
    s_prof_snap.scanline_bg_affine = cpu_prof.scanline_bg_affine_cycles / f;
    s_prof_snap.scanline_bg_bitmap = cpu_prof.scanline_bg_bitmap_cycles / f;
    s_prof_snap.scanline_obj = cpu_prof.scanline_obj_cycles / f;
    s_prof_snap.scanline_fx = cpu_prof.scanline_effect_cycles / f;
    s_prof_snap.scanline_order = cpu_prof.scanline_order_cycles / f;
    s_prof_snap.scanline_affine = cpu_prof.scanline_affine_cycles / f;
    s_prof_snap.scanline_blank = cpu_prof.scanline_blank_cycles / f;
    s_prof_snap.sound = cpu_prof.sound_cycles / f;
    s_prof_snap.dma = cpu_prof.dma_cycles / f;
    s_prof_snap.timer = cpu_prof.timer_cycles / f;
    s_prof_snap.serial = cpu_prof.serial_cycles / f;
    s_prof_snap.irq = cpu_prof.irq_cycles / f;
    /* dynarec overhead (sub-components of exec time) */
    s_prof_snap.dynarec_lookup = cpu_prof.dynarec_lookup_cycles / f;
    s_prof_snap.dynarec_translate = cpu_prof.dynarec_translate_cycles / f;
    s_prof_snap.dynarec_icache_sync = cpu_prof.dynarec_icache_sync_cycles / f;
    /* counts */
    s_prof_snap.arm_count = (cpu_prof.arm_count + cpu_prof.thumb_count) / f;
    s_prof_snap.thumb_count = cpu_prof.thumb_count / f;
    s_prof_snap.dynarec_lookup_hits = cpu_prof.dynarec_lookup_hits / f;
    s_prof_snap.dynarec_lookup_misses = cpu_prof.dynarec_lookup_misses / f;

    cpu_prof_reset();
}
#endif

/* Pre-allocated PSRAM buffer for state/save I/O (serialized access via command queue) */
static GPSP_EXTRAM_BSS uint8_t s_state_io_buf[GBA_SESSION_STATE_IO_BUF_SIZE] __attribute__((aligned(16)));

/* ── Background save task ────────────────────────────────────────── */

#define SAVE_TASK_STACK_SIZE  4096
#define SAVE_TASK_CORE        ((CONFIG_GPSP_EMULATION_CORE == 0) ? 1 : 0)

static TaskHandle_t s_save_task_handle;
static size_t s_save_pending_size;
static SemaphoreHandle_t s_save_done;  /* given = idle, taken = write in progress */

static void backup_save_task(void *param)
{
    (void)param;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        esp_err_t err = storage_write_save(s_session.rom_path,
                                           s_state_io_buf,
                                           s_save_pending_size);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Background save failed: %s", esp_err_to_name(err));
        }
        xSemaphoreGive(s_save_done);
    }
}

/* Block until any in-flight background save completes. */
static void save_task_await(void)
{
    if (!s_save_task_handle) return;
    xSemaphoreTake(s_save_done, portMAX_DELAY);
    xSemaphoreGive(s_save_done);
}

/* Copy backup → staging and kick the save task.
 * Waits for any prior save to drain before touching the staging buffer. */
static void save_task_kick(size_t size)
{
    xSemaphoreTake(s_save_done, portMAX_DELAY);
    memcpy(s_state_io_buf, gamepak_backup, size);
    s_save_pending_size = size;
    xTaskNotifyGive(s_save_task_handle);
}

static esp_err_t execute_command(gba_session_command_t *command);
static esp_err_t execute_reload(const gba_session_command_t *command);
static esp_err_t flush_backup_image(bool force);
static void flush_backup_async(void);

static size_t current_backup_size(void)
{
    switch (backup_type) {
        case BACKUP_EEPROM:
            return (eeprom_size == EEPROM_8_KBYTE) ? (8 * 1024) : 512;
        case BACKUP_FLASH:
            return (flash_bank_cnt == FLASH_SIZE_128KB) ? (128 * 1024) : (64 * 1024);
        case BACKUP_SRAM:
            return 32 * 1024;
        case BACKUP_UNKN:
        default:
            return sizeof(gamepak_backup);
    }
}

static void copy_path(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    strlcpy(dst, src, dst_size);
}

static bool path_is_set(const char *path)
{
    return path && path[0] != '\0';
}

static void clear_backup_dirty_flag(void)
{
    gamepak_backup_dirty = false;
}

static esp_err_t finalize_command(gba_session_command_t *command, esp_err_t status)
{
    if (command && command->result_ptr) {
        *command->result_ptr = status;
    }

    if (command && command->completion_task) {
        xTaskNotifyGive(command->completion_task);
    }

    return status;
}

static esp_err_t queue_command_and_wait(gba_session_command_t *command)
{
    esp_err_t result = ESP_OK;

    if (!command) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_session.control_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_session.emulation_task == NULL || xTaskGetCurrentTaskHandle() == s_session.emulation_task) {
        command->result_ptr = &result;
        return execute_command(command);
    }

    command->completion_task = xTaskGetCurrentTaskHandle();
    command->result_ptr = &result;

    if (xQueueSend(s_session.control_queue, command, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return result;
}

static esp_err_t load_state_file(unsigned slot)
{
    gba_session_state_file_header_t *header;
    uint8_t *buffer;
    uint8_t *state_data;
    size_t bytes_read = 0;
    size_t expected_size;
    esp_err_t result;

    if (!s_session.has_content || !path_is_set(s_session.rom_path)) {
        return ESP_ERR_INVALID_STATE;
    }

    buffer = s_state_io_buf;

    result = storage_read_state(s_session.rom_path, slot, buffer,
                                sizeof(*header) + GBA_STATE_MEM_SIZE + sizeof(gamepak_backup),
                                &bytes_read);
    if (result != ESP_OK) {
        return result;
    }

    if (bytes_read < sizeof(*header) + GBA_STATE_MEM_SIZE) {
        return ESP_FAIL;
    }

    header = (gba_session_state_file_header_t *)buffer;
    if (header->magic != GBA_SESSION_STATE_MAGIC ||
        header->version != GBA_SESSION_STATE_VERSION ||
        header->state_size != GBA_STATE_MEM_SIZE ||
        header->backup_size > sizeof(gamepak_backup)) {
        return ESP_FAIL;
    }

    expected_size = sizeof(*header) + header->state_size + header->backup_size;
    if (bytes_read != expected_size) {
        return ESP_FAIL;
    }

    memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
    state_data = buffer + sizeof(*header);
    memcpy(gamepak_backup, state_data + header->state_size, header->backup_size);
    clear_backup_dirty_flag();

    if (!gba_load_state(state_data)) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t save_state_file(unsigned slot)
{
    gba_session_state_file_header_t *header;
    uint8_t *buffer;
    uint8_t *state_data;
    size_t backup_size;
    size_t total_size;

    if (!s_session.has_content || !path_is_set(s_session.rom_path)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Ensure any in-flight async save finishes so s_state_io_buf is free.
     * No need to flush .sav here — the state file embeds backup data. */
    save_task_await();

    backup_size = current_backup_size();
    total_size = sizeof(*header) + GBA_STATE_MEM_SIZE + backup_size;
    buffer = s_state_io_buf;

    memset(buffer, 0, total_size);
    header = (gba_session_state_file_header_t *)buffer;
    header->magic = GBA_SESSION_STATE_MAGIC;
    header->version = GBA_SESSION_STATE_VERSION;
    header->state_size = GBA_STATE_MEM_SIZE;
    header->backup_size = (uint32_t)backup_size;

    state_data = buffer + sizeof(*header);
    gba_save_state(state_data);
    memcpy(state_data + GBA_STATE_MEM_SIZE, gamepak_backup, backup_size);

    if (storage_write_state(s_session.rom_path, slot, buffer, total_size) != ESP_OK) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t load_backup_image(const char *rom_path)
{
    size_t bytes_read = 0;
    esp_err_t err;

    memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));

    err = storage_read_save(rom_path, gamepak_backup, sizeof(gamepak_backup), &bytes_read);
    if (err == ESP_ERR_NOT_FOUND) {
        clear_backup_dirty_flag();
        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    clear_backup_dirty_flag();
    ESP_LOGI(TAG, "Save restored for %s (%u bytes)", rom_path, (unsigned)bytes_read);
    return ESP_OK;
}

static esp_err_t flush_backup_image(bool force)
{
    size_t backup_size;

    if (!s_session.has_content || !path_is_set(s_session.rom_path)) {
        return ESP_OK;
    }

    if (!force && !gamepak_backup_dirty) {
        return ESP_OK;
    }

    if (backup_type == BACKUP_UNKN) {
        return ESP_OK;
    }

    backup_size = current_backup_size();

    if (s_save_task_handle) {
        save_task_kick(backup_size);
        save_task_await();
    } else {
        /* Fallback: save task not yet started */
        if (storage_write_save(s_session.rom_path, gamepak_backup, backup_size) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    clear_backup_dirty_flag();
    s_session.last_autosave_us = esp_timer_get_time();
    return ESP_OK;
}

/* Fire-and-forget save: copy to staging, kick the save task, return immediately.
 * The SD card write runs on SERVICE_CORE while emulation continues. */
static void flush_backup_async(void)
{
    if (!s_session.has_content || !path_is_set(s_session.rom_path)) {
        return;
    }
    if (backup_type == BACKUP_UNKN) {
        return;
    }
    save_task_kick(current_backup_size());
    s_session.last_autosave_us = esp_timer_get_time();
}

static void load_builtin_bios_image(void)
{
    memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));  /* flash→RAM */
}

static esp_err_t apply_bios_image(const char *bios_path, bool *builtin_bios_active)
{
    bool use_builtin = true;

    if (path_is_set(bios_path)) {
        if (load_bios((char *)bios_path) == 0 && bios_rom[0] == 0x18) {
            ESP_LOGI(TAG, "Official BIOS loaded: %s", bios_path);
            use_builtin = false;
        } else {
            ESP_LOGW(TAG, "Falling back to built-in BIOS: %s", bios_path);
        }
    }

    if (use_builtin) {
        load_builtin_bios_image();
        ESP_LOGI(TAG, "Using built-in HLE BIOS");
    }

    if (builtin_bios_active) {
        *builtin_bios_active = use_builtin;
    }

    return ESP_OK;
}

static esp_err_t restore_previous_session(const gba_session_state_t *previous,
                                          const u8 *backup_snapshot)
{
    if (!previous || !previous->has_content) {
        return ESP_FAIL;
    }

    if (previous->builtin_bios_active) {
        load_builtin_bios_image();
    } else {
        apply_bios_image(previous->bios_path, NULL);
    }

    if (backup_snapshot) {
        memcpy(gamepak_backup, backup_snapshot, sizeof(gamepak_backup));
    }

    if (load_gamepak(NULL, previous->rom_path, 0, 0, 0) != 0) {
        return ESP_FAIL;
    }

    reset_gba();
    s_session.has_content = previous->has_content;
    s_session.builtin_bios_active = previous->builtin_bios_active;
    copy_path(s_session.rom_path, sizeof(s_session.rom_path), previous->rom_path);
    copy_path(s_session.bios_path, sizeof(s_session.bios_path), previous->bios_path);
    clear_backup_dirty_flag();
    return ESP_OK;
}

static esp_err_t execute_command(gba_session_command_t *command)
{
    if (!command) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (command->type) {
        case GBA_SESSION_CMD_SOFT_RESET:
            reset_gba();
            ESP_LOGI(TAG, "Soft reset complete");
            return finalize_command(command, ESP_OK);

        case GBA_SESSION_CMD_RELOAD:
            return finalize_command(command, execute_reload(command));

        case GBA_SESSION_CMD_SAVE_STATE:
            return finalize_command(command, save_state_file(command->state_slot));

        case GBA_SESSION_CMD_LOAD_STATE:
            return finalize_command(command, load_state_file(command->state_slot));

        case GBA_SESSION_CMD_SHUTDOWN:
            /* Emu loop exit path will do the final sync flush. */
            s_session.stop_requested = true;
            return finalize_command(command, ESP_OK);

        default:
            return finalize_command(command, ESP_ERR_NOT_SUPPORTED);
    }
}

static esp_err_t execute_reload(const gba_session_command_t *command)
{
    gba_session_state_t previous = s_session;
    bool next_builtin_bios = s_session.builtin_bios_active;
    const char *next_rom_path;
    const char *next_bios_path;
    u8 *backup_snapshot = NULL;
    int64_t t_total0;
    int64_t t_bios0;
    int64_t t_bios1;
    int64_t t_rom0;
    int64_t t_rom1;
    int64_t t_save0;
    int64_t t_save1;
    int64_t t_reset0;
    int64_t t_reset1;
    int64_t flush_us = 0;
    int64_t bios_us = 0;
    int64_t rom_us = 0;
    int64_t save_us = 0;
    int64_t reset_us = 0;

    if (!command) {
        return ESP_ERR_INVALID_ARG;
    }

    next_rom_path = command->reload_rom ? command->rom_path : s_session.rom_path;
    next_bios_path = command->reload_bios ? command->bios_path : s_session.bios_path;

    t_total0 = esp_timer_get_time();

    if (command->reload_rom && previous.has_content) {
        t_save0 = esp_timer_get_time();
        flush_backup_async();  /* kick SD write on service core */
        t_save1 = esp_timer_get_time();
        flush_us = t_save1 - t_save0;
    }

    if (command->reload_bios) {
        /* Overlap BIOS loading with the async save. */
        t_bios0 = esp_timer_get_time();
        apply_bios_image(next_bios_path, &next_builtin_bios);
        t_bios1 = esp_timer_get_time();
        bios_us = t_bios1 - t_bios0;
    }

    if (command->reload_rom) {
        save_task_await();  /* ensure s_state_io_buf is free */
        backup_snapshot = s_state_io_buf;
        memcpy(backup_snapshot, gamepak_backup, sizeof(gamepak_backup));
    }

    if (command->reload_rom) {
        if (command->clear_backup) {
            memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
        }

        t_rom0 = esp_timer_get_time();
        if (load_gamepak(NULL, next_rom_path, 0, 0, 0) != 0) {
            ESP_LOGE(TAG, "Failed to load ROM: %s", next_rom_path);
            restore_previous_session(&previous, backup_snapshot);
            return ESP_FAIL;
        }
        t_rom1 = esp_timer_get_time();
        rom_us = t_rom1 - t_rom0;

        ESP_LOGI(TAG, "ROM loaded: %s (%u bytes)", next_rom_path, (unsigned)gamepak_size);

        t_save0 = esp_timer_get_time();
        if (load_backup_image(next_rom_path) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to restore save for %s", next_rom_path);
        }
        t_save1 = esp_timer_get_time();
        save_us = t_save1 - t_save0;
    }

    t_reset0 = esp_timer_get_time();
    reset_gba();
#ifdef HAVE_DYNAREC
    flush_dynarec_caches();
#endif
    t_reset1 = esp_timer_get_time();
    reset_us = t_reset1 - t_reset0;

    s_session.has_content = true;
    s_session.builtin_bios_active = next_builtin_bios;
    s_session.stop_requested = false;
    copy_path(s_session.rom_path, sizeof(s_session.rom_path), next_rom_path);
    copy_path(s_session.bios_path, sizeof(s_session.bios_path), next_bios_path);
    s_session.last_autosave_us = esp_timer_get_time();

    ESP_LOGI(TAG,
             "Content load: total %lld us | flush %lld us bios %lld us rom %lld us save %lld us reset %lld us",
             (long long)(s_session.last_autosave_us - t_total0),
             (long long)flush_us,
             (long long)bios_us,
             (long long)rom_us,
             (long long)save_us,
             (long long)reset_us);
    ESP_LOGI(TAG, "GBA session reset complete");
    return ESP_OK;
}

esp_err_t gba_session_init(const gba_session_boot_config_t *config)
{
    gba_session_command_t command;

    if (!config || !path_is_set(config->rom_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_session.control_queue) {
        s_session.control_queue = xQueueCreate(GBA_SESSION_QUEUE_LEN, sizeof(gba_session_command_t));
        if (!s_session.control_queue) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Start background save task on the service core */
    if (!s_save_task_handle) {
        s_save_done = xSemaphoreCreateBinary();
        if (!s_save_done) {
            return ESP_ERR_NO_MEM;
        }
        xSemaphoreGive(s_save_done);  /* idle initially */

        BaseType_t ret = xTaskCreatePinnedToCore(
            backup_save_task, "gba_save",
            SAVE_TASK_STACK_SIZE, NULL,
            configMAX_PRIORITIES - 4,
            &s_save_task_handle,
            SAVE_TASK_CORE);
        if (ret != pdPASS) {
            ESP_LOGW(TAG, "Background save task failed; saves will block emu");
            vSemaphoreDelete(s_save_done);
            s_save_done = NULL;
        }
    }

    init_main();
    init_sound();

    if (!gba_screen_pixels) {
        gba_screen_pixels = av_pipeline_video_buffer();
    }

    {
        u32 rom_buf_count = init_gamepak_buffer();
        ESP_LOGI(TAG, "ROM buffers: %u MB in PSRAM", (unsigned)rom_buf_count);
    }

    memset(&command, 0, sizeof(command));
    command.type = GBA_SESSION_CMD_RELOAD;
    command.reload_rom = true;
    command.reload_bios = true;
    command.clear_backup = true;
    copy_path(command.rom_path, sizeof(command.rom_path), config->rom_path);
    copy_path(command.bios_path, sizeof(command.bios_path), config->bios_path);

    if (execute_reload(&command) != ESP_OK) {
        return ESP_FAIL;
    }

    s_session.initialized = true;
    return ESP_OK;
}

esp_err_t gba_session_request_soft_reset(void)
{
    gba_session_command_t command;

    if (!s_session.control_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&command, 0, sizeof(command));
    command.type = GBA_SESSION_CMD_SOFT_RESET;

    if (xQueueSend(s_session.control_queue, &command, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t gba_session_request_reload(const gba_session_reload_request_t *request)
{
    gba_session_command_t command;

    if (!request) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_session.control_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&command, 0, sizeof(command));
    command.type = GBA_SESSION_CMD_RELOAD;
    command.reload_rom = request->reload_rom;
    command.reload_bios = request->reload_bios;
    command.clear_backup = request->clear_backup;

    copy_path(command.rom_path, sizeof(command.rom_path),
              request->reload_rom ? request->rom_path : s_session.rom_path);
    copy_path(command.bios_path, sizeof(command.bios_path),
              request->reload_bios ? request->bios_path : s_session.bios_path);

    if (xQueueSend(s_session.control_queue, &command, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t gba_session_save_state(unsigned slot)
{
    gba_session_command_t command;

    memset(&command, 0, sizeof(command));
    command.type = GBA_SESSION_CMD_SAVE_STATE;
    command.state_slot = slot;

    return queue_command_and_wait(&command);
}

esp_err_t gba_session_load_state(unsigned slot)
{
    gba_session_command_t command;

    memset(&command, 0, sizeof(command));
    command.type = GBA_SESSION_CMD_LOAD_STATE;
    command.state_slot = slot;

    return queue_command_and_wait(&command);
}

esp_err_t gba_session_shutdown(void)
{
    gba_session_command_t command;

    memset(&command, 0, sizeof(command));
    command.type = GBA_SESSION_CMD_SHUTDOWN;

    return queue_command_and_wait(&command);
}

esp_err_t gba_session_process_pending(void)
{
    gba_session_command_t command;
    BaseType_t queue_status;

    if (!s_session.control_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    do {
        queue_status = xQueueReceive(s_session.control_queue, &command, 0);
        if (queue_status != pdTRUE) {
            break;
        }

        if (execute_command(&command) != ESP_OK) {
            return ESP_FAIL;
        }
    } while (queue_status == pdTRUE);

    return ESP_OK;
}

void gba_emulation_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "Starting emulation now");
    s_session.emulation_task = xTaskGetCurrentTaskHandle();

#ifdef HAVE_DYNAREC
    /* init_emitter must run here (not in init_main on the main task)
       because dynarec translation recurses and needs the large emu stack. */
    init_emitter(gamepak_must_swap());
#endif

    s_fps_timer_us = esp_timer_get_time();
    gba_session_perf_reset();

    gba_screen_pixels = av_pipeline_video_buffer();

    while (1) {
        s_frame_start_us = esp_timer_get_time();
        int64_t t0, t1, t_acq0, t_acq1;

        /* Wait for previous frame's async PPA + VSYNC swap.
         * First iteration: no pending PPA, returns immediately. */
        t_acq0 = esp_timer_get_time();
        av_pipeline_begin_frame();
        t_acq1 = esp_timer_get_time();

        if (gba_session_process_pending() != ESP_OK) {
            ESP_LOGW(TAG, "Failed to process pending GBA session request");
        }

        if (s_session.stop_requested) {
            break;
        }

        if (gamepak_backup_dirty) {
            /* Record when the dirty burst started, then clear the flag
             * so we can detect when writes stop. */
            if (!s_session.backup_dirty_since_us) {
                s_session.backup_dirty_since_us = s_frame_start_us;
            }
            gamepak_backup_dirty = false;
        } else if (s_session.backup_dirty_since_us &&
                   (s_frame_start_us - s_session.last_autosave_us) >= GBA_SESSION_AUTOSAVE_PERIOD_US) {
            /* Dirty burst ended and cooldown elapsed — async flush on service core. */
            flush_backup_async();
            s_session.backup_dirty_since_us = 0;
        }

        {
            uint16_t keys = input_driver_read();
            keys |= web_server_input_read();

            /* Check P1CNT keypad interrupt on new presses (matches libretro) */
            uint16_t old_keys = ~read_ioreg(REG_P1) & 0x3FF;
            if ((keys | old_keys) != old_keys) {
                u32 p1_cnt = read_ioreg(REG_P1CNT);
                if ((p1_cnt >> 14) & 0x01) {
                    u32 key_intersection = (p1_cnt & keys) & 0x3FF;
                    if ((p1_cnt >> 15)
                        ? (key_intersection == (p1_cnt & 0x3FF))
                        : (key_intersection != 0)) {
                        flag_interrupt(IRQ_KEYPAD);
                        check_and_raise_interrupts();
                    }
                }
            }

            write_ioreg(REG_P1, ~keys & 0x3FF);
        }
        skip_next_frame = 0;

#ifdef CPU_PROFILE_STATS
        u32 scanline_cyc_before = cpu_prof.scanline_cycles;
#endif
        t0 = esp_timer_get_time();

#ifdef HAVE_DYNAREC
        if (dynarec_enable) {
            CPU_PROF_TOTAL_BEGIN();
            CPU_PROF_SCOPE_BEGIN(dynarec_total_begin);
            execute_arm_translate(execute_cycles);
            CPU_PROF_SCOPE_ACC(dynarec_total_cycles, dynarec_total_begin);
            CPU_PROF_TOTAL_END();
            CPU_PROF_INC(dynarec_frames);
            CPU_PROF_FRAME();
        } else
#endif
        {
            clear_gamepak_stickybits();
            execute_arm(execute_cycles);
        }

        t1 = esp_timer_get_time();

        {
            int64_t t_sub;

            if (av_pipeline_submit_frame(skip_next_frame != 0) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to submit AV frame");
                vTaskDelete(NULL);
                return;
            }

            t_sub = esp_timer_get_time();

            if (s_session.stop_requested) {
                av_pipeline_begin_frame(); /* drain pending PPA */
                break;
            }

            {
#ifdef CPU_PROFILE_STATS
                int64_t render_frame_us = (int64_t)(cpu_prof.scanline_cycles - scanline_cyc_before)
                                        / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
#else
                int64_t render_frame_us = 0;
#endif
                gba_session_perf_push_single_core(t1 - t0,
                                                  render_frame_us,
                                                  t_sub - t1,
                                                  t_acq1 - t_acq0);
            }

            s_fps_counter++;
            {
                /* Use t_acq1 (post-begin_frame, VSYNC-aligned) for accurate FPS */
                if (t_acq1 - s_fps_timer_us >= 1000000) {
                    int64_t dt = t_acq1 - s_fps_timer_us;
                    s_fps_last_x10 = (uint32_t)((uint64_t)s_fps_counter * 10000000 / dt);
                    if (!gpsp_web_server_enabled) {
                        ESP_LOGI(TAG, "FPS: %u.%u | stats window %u frames",
                                 (unsigned)(s_fps_last_x10 / 10),
                                 (unsigned)(s_fps_last_x10 % 10),
                                 (unsigned)GBA_SESSION_STATS_WINDOW);
                        gba_session_perf_log_single_core();
                    }
#ifdef CPU_PROFILE_STATS
                    gba_session_snapshot_cpu_prof();
#endif
                    s_fps_counter = 0;
                    s_fps_timer_us = t_acq1;
                }
            }

            if (!av_pipeline_audio_enabled()) {
                int64_t elapsed_us = esp_timer_get_time() - s_frame_start_us;
                int64_t target_us = 16742;
                if (elapsed_us < target_us) {
                    vTaskDelay(pdMS_TO_TICKS((target_us - elapsed_us) / 1000));
                }
            }
        }
    }

    flush_backup_image(true);
    memory_term();
    s_session.emulation_task = NULL;
    s_session.has_content = false;
    s_session.initialized = false;
    ESP_LOGI(TAG, "Emulation task stopped");
    vTaskDelete(NULL);
}

/* ── Public JSON stats snapshot ──────────────────────────────────── */

static int json_stat(char *p, char *end, const char *name,
                     const frame_stat_window_t *w)
{
    frame_stat_summary_t s = frame_stat_window_summarize(w);
    int n = snprintf(p, end - p,
                     "\"%s\":{\"avg\":%lld,\"p99\":%lld,\"n\":%u},",
                     name,
                     (long long)s.avg_us,
                     (long long)s.p99_us,
                     (unsigned)s.sample_count);
    return (n > 0 && p + n < end) ? n : 0;
}

int gba_session_stats_json(char *buf, size_t buf_size)
{
    if (!buf || buf_size < 4) return -1;
    char *p = buf;
    char *end = buf + buf_size - 1;

    *p++ = '{';

    p += snprintf(p, end - p, "\"fps_x10\":%u,\"heap_kb\":%u,",
                  (unsigned)s_fps_last_x10,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
    if (p >= end) goto trunc;

    p += json_stat(p, end, "cpu",     &s_perf_stats.cpu_us);
    p += json_stat(p, end, "render",  &s_perf_stats.render_us);
    p += json_stat(p, end, "submit",  &s_perf_stats.submit_us);
    p += json_stat(p, end, "acquire", &s_perf_stats.acquire_us);

#ifdef CPU_PROFILE_STATS
    if (s_prof_snap.frames > 0 && s_prof_snap.total > 0) {
        int n = snprintf(p, end - p,
            "\"prof\":{"
            "\"total\":%u,\"exec\":%u,\"update\":%u,"
            "\"scan\":%u,"
            "\"bg\":%u,\"bg_tf\":%u,\"bg_tm\":%u,\"bg_af\":%u,\"bg_bm\":%u,"
            "\"obj\":%u,\"fx\":%u,\"ord\":%u,\"aff\":%u,\"blank\":%u,"
            "\"sound\":%u,\"dma\":%u,\"timer\":%u,\"serial\":%u,\"irq\":%u,"
            "\"drc_lkup\":%u,\"drc_xlat\":%u,\"drc_sync\":%u,"
            "\"insn\":%u,\"lkup_hit\":%u,\"lkup_miss\":%u},",
            (unsigned)s_prof_snap.total,
            (unsigned)s_prof_snap.exec,
            (unsigned)s_prof_snap.update,
            (unsigned)s_prof_snap.scanline,
            (unsigned)s_prof_snap.scanline_bg,
            (unsigned)s_prof_snap.scanline_bg_text_fast,
            (unsigned)s_prof_snap.scanline_bg_text_mosaic,
            (unsigned)s_prof_snap.scanline_bg_affine,
            (unsigned)s_prof_snap.scanline_bg_bitmap,
            (unsigned)s_prof_snap.scanline_obj,
            (unsigned)s_prof_snap.scanline_fx,
            (unsigned)s_prof_snap.scanline_order,
            (unsigned)s_prof_snap.scanline_affine,
            (unsigned)s_prof_snap.scanline_blank,
            (unsigned)s_prof_snap.sound,
            (unsigned)s_prof_snap.dma,
            (unsigned)s_prof_snap.timer,
            (unsigned)s_prof_snap.serial,
            (unsigned)s_prof_snap.irq,
            (unsigned)s_prof_snap.dynarec_lookup,
            (unsigned)s_prof_snap.dynarec_translate,
            (unsigned)s_prof_snap.dynarec_icache_sync,
            (unsigned)s_prof_snap.arm_count,
            (unsigned)s_prof_snap.dynarec_lookup_hits,
            (unsigned)s_prof_snap.dynarec_lookup_misses);
        if (n > 0 && p + n < end) p += n;
    }
#endif

    if (p >= end) goto trunc;
    if (p > buf + 1 && *(p - 1) == ',') p--;
    *p++ = '}';
    *p = '\0';
    return (int)(p - buf);

trunc:
    buf[0] = '\0';
    return -1;
}