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
#include "gba_memory.h"
#include "gba_session.h"
#include "input_driver.h"
#include "main.h"
#include "savestate.h"
#include "sound.h"
#include "storage.h"
#include "video.h"

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

    if (!command) {
        return ESP_ERR_INVALID_ARG;
    }

    next_rom_path = command->reload_rom ? command->rom_path : s_session.rom_path;
    next_bios_path = command->reload_bios ? command->bios_path : s_session.bios_path;

    if (!path_is_set(next_rom_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (command->reload_rom && previous.has_content) {
        if (flush_backup_image(true) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to flush save before ROM reload");
            return ESP_FAIL;
        }
    }

    if (command->reload_rom) {
        backup_snapshot = s_state_io_buf;
        memcpy(backup_snapshot, gamepak_backup, sizeof(gamepak_backup));
    }

    if (command->reload_bios) {
        apply_bios_image(next_bios_path, &next_builtin_bios);
    }

    if (command->reload_rom) {
        if (command->clear_backup) {
            memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
        }

        if (load_gamepak(NULL, next_rom_path, 0, 0, 0) != 0) {
            ESP_LOGE(TAG, "Failed to load ROM: %s", next_rom_path);
            restore_previous_session(&previous, backup_snapshot);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "ROM loaded: %s (%u bytes)", next_rom_path, (unsigned)gamepak_size);

        if (load_backup_image(next_rom_path) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to restore save for %s", next_rom_path);
        }
    }

    reset_gba();

    s_session.has_content = true;
    s_session.builtin_bios_active = next_builtin_bios;
    s_session.stop_requested = false;
    copy_path(s_session.rom_path, sizeof(s_session.rom_path), next_rom_path);
    copy_path(s_session.bios_path, sizeof(s_session.bios_path), next_bios_path);
    s_session.last_autosave_us = esp_timer_get_time();

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
    uint32_t slot_index;
    u16 *video_buffer;

    (void)param;

    ESP_LOGI(TAG, "Emulation task started on core %d", xPortGetCoreID());
    s_session.emulation_task = xTaskGetCurrentTaskHandle();

    s_fps_timer_us = esp_timer_get_time();

    gba_screen_pixels = av_pipeline_default_video_buffer();

    if (av_pipeline_acquire_slot(&slot_index, &video_buffer, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to acquire initial AV slot");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        s_frame_start_us = esp_timer_get_time();

        if (gba_session_process_pending() != ESP_OK) {
            ESP_LOGW(TAG, "Failed to process pending GBA session request");
        }

        if (s_session.stop_requested) {
            av_pipeline_release_slot(slot_index, portMAX_DELAY);
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

#ifdef HAVE_DYNAREC
        if (dynarec_enable) {
            execute_arm_translate(execute_cycles);
        } else
#endif
        {
            clear_gamepak_stickybits();
            execute_arm(execute_cycles);
        }

        if (!skip_next_frame) {
            memcpy(video_buffer, gba_screen_pixels,
                   GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * sizeof(u16));
        }

        if (av_pipeline_submit_slot(slot_index, skip_next_frame != 0, portMAX_DELAY) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to submit AV slot");
            vTaskDelete(NULL);
            return;
        }

        if (s_session.stop_requested) {
            break;
        }

        if (av_pipeline_acquire_slot(&slot_index, &video_buffer, portMAX_DELAY) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to acquire AV slot");
            vTaskDelete(NULL);
            return;
        }

        s_fps_counter++;
        {
            int64_t now = esp_timer_get_time();
            if (now - s_fps_timer_us >= 1000000) {
                ESP_LOGI(TAG, "FPS: %u | Free heap: %u KB",
                         (unsigned)s_fps_counter,
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
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

    flush_backup_image(true);
    memory_term();
    s_session.emulation_task = NULL;
    s_session.has_content = false;
    s_session.initialized = false;
    ESP_LOGI(TAG, "Emulation task stopped");
    vTaskDelete(NULL);
}