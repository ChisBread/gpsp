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

#ifdef DUAL_CORE_PPU
#include "ppu_pipeline.h"
#endif

#define GBA_SESSION_PATH_MAX 512
#define GBA_SESSION_QUEUE_LEN 4
#define GBA_SESSION_AUTOSAVE_PERIOD_US (5 * 1000 * 1000)
#define GBA_SESSION_STATE_MAGIC 0x53545347u
#define GBA_SESSION_STATE_VERSION 1u
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
} gba_session_state_t;

static const char *TAG = "gpsp_session";
static gba_session_state_t s_session;
static int64_t s_frame_start_us;
static uint32_t s_fps_counter;
static int64_t s_fps_timer_us;

#ifdef CPU_PROFILE_STATS
static void log_scanline_breakdown(void)
{
    if (cpu_prof.frames == 0) {
        return;
    }

    const u32 frames = cpu_prof.frames;
    const u32 order = cpu_prof.scanline_order_cycles / frames;
    const u32 bg = cpu_prof.scanline_bg_cycles / frames;
    const u32 obj = cpu_prof.scanline_obj_cycles / frames;
    const u32 fx = cpu_prof.scanline_effect_cycles / frames;
    const u32 blank = cpu_prof.scanline_blank_cycles / frames;
    const u32 affine = cpu_prof.scanline_affine_cycles / frames;
    const u32 render_total = order + bg + obj + fx + blank + affine;
    const u32 bg_text_fast = cpu_prof.scanline_bg_text_fast_cycles / frames;
    const u32 bg_text_mosaic = cpu_prof.scanline_bg_text_mosaic_cycles / frames;
    const u32 bg_affine = cpu_prof.scanline_bg_affine_cycles / frames;
    const u32 bg_bitmap = cpu_prof.scanline_bg_bitmap_cycles / frames;

    ESP_LOGI(TAG,
             "RSCAN cyc/frame: total %u | ord %u bg %u obj %u fx %u blank %u aff %u",
             (unsigned)render_total,
             (unsigned)order,
             (unsigned)bg,
             (unsigned)obj,
             (unsigned)fx,
             (unsigned)blank,
             (unsigned)affine);
    ESP_LOGI(TAG,
             "BG cyc/frame: text_fast %u text_mosaic %u affine %u bitmap %u",
             (unsigned)bg_text_fast,
             (unsigned)bg_text_mosaic,
             (unsigned)bg_affine,
             (unsigned)bg_bitmap);
}
#endif

/* Pre-allocated PSRAM buffer for state/save I/O (serialized access via command queue) */
static GPSP_EXTRAM_BSS uint8_t s_state_io_buf[GBA_SESSION_STATE_IO_BUF_SIZE] __attribute__((aligned(16)));

static esp_err_t execute_command(gba_session_command_t *command);
static esp_err_t execute_reload(const gba_session_command_t *command);
static esp_err_t flush_backup_image(bool force);

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

    if (flush_backup_image(true) != ESP_OK) {
        return ESP_FAIL;
    }

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
    if (storage_write_save(s_session.rom_path, gamepak_backup, backup_size) != ESP_OK) {
        return ESP_FAIL;
    }

    clear_backup_dirty_flag();
    s_session.last_autosave_us = esp_timer_get_time();
    return ESP_OK;
}

static void load_builtin_bios_image(void)
{
    memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
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
            if (flush_backup_image(true) != ESP_OK) {
                return finalize_command(command, ESP_FAIL);
            }
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
        if (flush_backup_image(true) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to flush save before ROM reload");
            return ESP_FAIL;
        }
        t_save1 = esp_timer_get_time();
        flush_us = t_save1 - t_save0;
    }

    if (command->reload_rom) {
        backup_snapshot = s_state_io_buf;
        memcpy(backup_snapshot, gamepak_backup, sizeof(gamepak_backup));
    }

    if (command->reload_bios) {
        t_bios0 = esp_timer_get_time();
        apply_bios_image(next_bios_path, &next_builtin_bios);
        t_bios1 = esp_timer_get_time();
        bios_us = t_bios1 - t_bios0;
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

    init_main();
    init_sound();

    if (!gba_screen_pixels) {
        gba_screen_pixels = av_pipeline_default_video_buffer();
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
#ifndef DUAL_CORE_PPU
    uint32_t slot_index;
    u16 *video_buffer;
#endif

    (void)param;

    ESP_LOGI(TAG, "Starting emulation now");
    s_session.emulation_task = xTaskGetCurrentTaskHandle();

#ifdef HAVE_DYNAREC
    /* init_emitter must run here (not in init_main on the main task)
       because dynarec translation recurses and needs the large emu stack. */
    init_emitter(gamepak_must_swap());
#endif

    s_fps_timer_us = esp_timer_get_time();

#ifdef DUAL_CORE_PPU
    /* The render task owns gba_screen_pixels.  The emu core does not
     * render, so it does not need it.  Set a dummy buffer so any
     * stray accesses don't crash (e.g. init code). */
    gba_screen_pixels = av_pipeline_default_video_buffer();
#else
    gba_screen_pixels = av_pipeline_default_video_buffer();

    if (av_pipeline_acquire_slot(&slot_index, &video_buffer, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to acquire initial AV slot");
        vTaskDelete(NULL);
        return;
    }
#endif

    while (1) {
        s_frame_start_us = esp_timer_get_time();
        int64_t t0, t1;
#ifdef DUAL_CORE_PPU
        int64_t t_wait;
#endif

        if (gba_session_process_pending() != ESP_OK) {
            ESP_LOGW(TAG, "Failed to process pending GBA session request");
        }

        if (s_session.stop_requested) {
#ifndef DUAL_CORE_PPU
            av_pipeline_release_slot(slot_index, portMAX_DELAY);
#endif
            break;
        }

        if (gamepak_backup_dirty &&
            (s_frame_start_us - s_session.last_autosave_us) >= GBA_SESSION_AUTOSAVE_PERIOD_US) {
            if (flush_backup_image(false) != ESP_OK) {
                ESP_LOGW(TAG, "Periodic save flush failed");
            }
        }

        {
            uint16_t keys = input_driver_read();
            write_ioreg(REG_P1, ~keys & 0x3FF);
        }
        skip_next_frame = 0;

#ifdef DUAL_CORE_PPU
        /* Wait for the render task to finish the previous frame before
         * we start writing new scanline descriptors. */
        ppu_pipeline_begin_frame();
        t_wait = esp_timer_get_time();
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

#ifndef DUAL_CORE_PPU
        {
            int64_t t2, t3, t4;

            if (!skip_next_frame) {
                memcpy(video_buffer, gba_screen_pixels,
                       GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * sizeof(u16));
            }

            t2 = esp_timer_get_time();

            if (av_pipeline_submit_slot(slot_index, skip_next_frame != 0, portMAX_DELAY) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to submit AV slot");
                vTaskDelete(NULL);
                return;
            }

            t3 = esp_timer_get_time();

            if (s_session.stop_requested) {
                break;
            }

            if (av_pipeline_acquire_slot(&slot_index, &video_buffer, portMAX_DELAY) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to acquire AV slot");
                vTaskDelete(NULL);
                return;
            }

            t4 = esp_timer_get_time();

            s_fps_counter++;
            {
                int64_t now = esp_timer_get_time();
                if (now - s_fps_timer_us >= 1000000) {
                    ESP_LOGI(TAG, "FPS: %u | cpu: %lld us | copy: %lld us | submit: %lld us | acquire: %lld us | heap: %u KB",
                             (unsigned)s_fps_counter,
                             (long long)(t1 - t0),
                             (long long)(t2 - t1),
                             (long long)(t3 - t2),
                             (long long)(t4 - t3),
                             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
#ifdef CPU_PROFILE_STATS
                    cpu_prof_print();
#endif
                    s_fps_counter = 0;
                    s_fps_timer_us = now;
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
#else /* DUAL_CORE_PPU */
        /* Frame is done — scanlines + end_frame were pushed inside
         * update_gba().  Just log stats. */
        s_fps_counter++;
        {
            int64_t now = esp_timer_get_time();
            if (now - s_fps_timer_us >= 1000000) {
                int64_t r_scan, r_video, r_audio;
                int64_t w_render, w_buf, w_pace;
                int64_t emu_wall;
                uint32_t a_drop, a_qpeak;
                ppu_pipeline_get_render_stats(&r_scan, &r_video, &r_audio);
                ppu_pipeline_get_wait_stats(&w_render, &w_buf, &w_pace,
                                            &a_drop, &a_qpeak);
                emu_wall = t1 - s_frame_start_us;
                ESP_LOGI(TAG, "FPS: %u | emu: %lld us | cpu: %lld us | wait: %lld us (rd %lld buf %lld hz60 %lld) | R: scan %lld vid %lld aud %lld us | A: drop %u qpk %u | heap: %u KB",
                         (unsigned)s_fps_counter,
                         (long long)emu_wall,
                         (long long)(t1 - t0),
                         (long long)(t_wait - s_frame_start_us),
                         (long long)w_render,
                         (long long)w_buf,
                         (long long)w_pace,
                         (long long)r_scan, (long long)r_video, (long long)r_audio,
                         (unsigned)a_drop,
                         (unsigned)a_qpeak,
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
#ifdef CPU_PROFILE_STATS
                log_scanline_breakdown();
                cpu_prof_print();
#endif
                s_fps_counter = 0;
                s_fps_timer_us = now;
            }
        }

#endif /* DUAL_CORE_PPU */
    }

    flush_backup_image(true);
#ifdef DUAL_CORE_PPU
    ppu_pipeline_deinit();
#endif
    memory_term();
    s_session.emulation_task = NULL;
    s_session.has_content = false;
    s_session.initialized = false;
    ESP_LOGI(TAG, "Emulation task stopped");
    vTaskDelete(NULL);
}