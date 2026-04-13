/*
 * gpsp-esp32p4 — GBA emulator for ESP32-P4
 * Target board: JC4880P443C_I_W
 *
 * Main application entry point (ESP-IDF app_main)
 *
 * This file bridges the ESP32-P4 platform drivers with the gpsp
 * emulation core. It initializes hardware (display, audio, input, storage),
 * loads a ROM, and runs the emulation loop.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"

/* gpsp core headers */
#include "common.h"
#include "main.h"
#include "gba_memory.h"
#include "sound.h"

/* ESP32-P4 platform drivers (JC4880) */
#include "video_driver.h"
#include "audio_driver.h"
#include "av_pipeline.h"
#include "gba_session.h"
#include "input_driver.h"
#include "c6_remote.h"
#include "runtime_config.h"
#include "storage.h"
#include "web_server.h"

static const char *TAG = "gpsp_main";

/* ---- Globals expected by gpsp core ---- */
u32 skip_next_frame = 0;
int dynarec_enable = 1;     /* Start with dynarec mode */
int sprite_limit = 1;
boot_mode selected_boot_mode = boot_game;

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;

#define GPSP_AUDIO_SOURCE_RATE   GBA_SOUND_FREQUENCY
#define GPSP_AUDIO_OUTPUT_RATE   CONFIG_GPSP_AUDIO_SAMPLE_RATE
#define SERVICE_CORE             ((CONFIG_GPSP_EMULATION_CORE == 0) ? 1 : 0)

/* ================================================================
 * Platform initialization
 * ================================================================ */

static esp_err_t init_platform(bool *audio_ready)
{
    esp_err_t err;

    if (!audio_ready) {
        return ESP_ERR_INVALID_ARG;
    }

    *audio_ready = false;

    /* ---- Report memory status ---- */
    ESP_LOGI(TAG, "Free internal RAM: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ESP_LOGI(TAG, "Free PSRAM: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* ---- Initialize storage (SD card — SDMMC Slot 0 with LDO power) ---- */
    err = storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Storage init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = gpsp_runtime_config_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Runtime config init failed: %s", esp_err_to_name(err));
    }

    /* ---- Initialize display (ST7701 MIPI-DSI + backlight) ---- */
    video_driver_config_t video_config = {
        .lcd_h_res = CONFIG_GPSP_LCD_H_RES,
        .lcd_v_res = CONFIG_GPSP_LCD_V_RES,
        .use_ppa_scaling = CONFIG_GPSP_USE_PPA_SCALING,
        .num_fbs = CONFIG_GPSP_NUM_FB,
    };
    err = video_driver_init(&video_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Video init failed: %s", esp_err_to_name(err));
        return err;
    }
    video_driver_set_brightness(CONFIG_GPSP_LCD_BRIGHTNESS);

    /* ---- Initialize audio (ES8311 codec via I2C + I2S) ---- */
    audio_driver_config_t audio_config = {
        .sample_rate = GPSP_AUDIO_OUTPUT_RATE,
        .source_sample_rate = GPSP_AUDIO_SOURCE_RATE,
    };
    ESP_LOGI(TAG, "Audio path: gpsp PCM %d Hz -> codec output %d Hz",
             GPSP_AUDIO_SOURCE_RATE, GPSP_AUDIO_OUTPUT_RATE);
    err = audio_driver_init(&audio_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Audio init failed: %s (continuing without sound)",
                 esp_err_to_name(err));
        /* Audio failure is non-fatal */
    } else {
        audio_driver_set_volume(CONFIG_GPSP_AUDIO_VOLUME);
        *audio_ready = true;
    }

    /* ---- Initialize input (GPIO buttons) ---- */
    input_driver_config_t input_config = {
        .gpio_a      = CONFIG_GPSP_GPIO_A,
        .gpio_b      = CONFIG_GPSP_GPIO_B,
        .gpio_select = CONFIG_GPSP_GPIO_SELECT,
        .gpio_start  = CONFIG_GPSP_GPIO_START,
        .gpio_right  = CONFIG_GPSP_GPIO_RIGHT,
        .gpio_left   = CONFIG_GPSP_GPIO_LEFT,
        .gpio_up     = CONFIG_GPSP_GPIO_UP,
        .gpio_down   = CONFIG_GPSP_GPIO_DOWN,
        .gpio_r      = CONFIG_GPSP_GPIO_R,
        .gpio_l      = CONFIG_GPSP_GPIO_L,
    };
    err = input_driver_init(&input_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Input init failed: %s", esp_err_to_name(err));
        /* Input failure is non-fatal — may have no buttons configured */
    }

    return ESP_OK;
}

/* ================================================================
 * app_main — ESP-IDF entry point
 * ================================================================ */

void app_main(void)
{
    bool audio_ready;
    char bios_path[64];

    ESP_LOGI(TAG, "===================================");
    ESP_LOGI(TAG, "  gpsp-esp32p4 GBA Emulator");
    ESP_LOGI(TAG, "  Platform: ESP32-P4 @ %d MHz",
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    ESP_LOGI(TAG, "===================================");

    /* Report PSRAM availability */
    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM: %u MB", (unsigned)(psram_size / (1024 * 1024)));

    if (psram_size < 8 * 1024 * 1024) {
        ESP_LOGE(TAG, "Insufficient PSRAM! Need at least 8 MB, have %u MB",
                 (unsigned)(psram_size / (1024 * 1024)));
        return;
    }

    /* Initialize all platform drivers */
    esp_err_t err = init_platform(&audio_ready);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Platform init failed, halting");
        return;
    }

    {
        av_pipeline_config_t av_config = {
            .audio_enabled = audio_ready,
        };

        err = av_pipeline_init(&av_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AV pipeline init failed, halting");
            return;
        }
    }

    snprintf(bios_path, sizeof(bios_path), "%s/gba_bios.bin", STORAGE_MOUNT_POINT);

    /* Try to boot from the most recent game first, fall back to default */
    const char *boot_rom = CONFIG_GPSP_ROM_PATH;
    static char recent_rom_path[512];
    {
        char **recent_list = NULL;
        size_t recent_count = 0;
        storage_read_recent_list(&recent_list, &recent_count, 10);
        if (recent_count > 0 && recent_list && recent_list[0]) {
            struct stat st;
            if (stat(recent_list[0], &st) == 0 && S_ISREG(st.st_mode)) {
                strlcpy(recent_rom_path, recent_list[0], sizeof(recent_rom_path));
                boot_rom = recent_rom_path;
                ESP_LOGI(TAG, "Auto-boot from recent: %s", boot_rom);
            } else {
                ESP_LOGW(TAG, "Recent ROM not found: %s, using default", recent_list[0]);
            }
        }
        if (recent_list) {
            for (size_t i = 0; i < recent_count; i++) free(recent_list[i]);
            free(recent_list);
        }
    }

    gba_session_boot_config_t session_config = {
        .rom_path = boot_rom,
        .bios_path = bios_path,
    };

    ESP_LOGI(TAG,
             "GBA config: rom=%s bios=%s boot=%s dynarec=%d sprite_limit=%d audio=%d",
             session_config.rom_path,
             session_config.bios_path,
             selected_boot_mode == boot_bios ? "bios" : "game",
             dynarec_enable ? 1 : 0,
             sprite_limit ? 1 : 0,
             audio_ready ? 1 : 0);
    ESP_LOGI(TAG, "GBA renderer: output_core=%d c6_remote=%d",
             SERVICE_CORE,
             CONFIG_GPSP_ENABLE_C6_REMOTE ? 1 : 0);

    err = gba_session_init(&session_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GBA init failed, halting");
        return;
    }

    err = c6_remote_start_task(SERVICE_CORE, configMAX_PRIORITIES - 3);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create C6 remote task: %s", esp_err_to_name(err));
        return;
    }

    /* Start embedded HTTP/WebSocket server after network is ready (async, render core) */
    if (gpsp_web_server_enabled) {
        web_server_start_async(SERVICE_CORE);
    } else {
        ESP_LOGI(TAG, "Web server disabled by config");
    }

    ESP_LOGI(TAG, "Before emu task: free internal RAM %u KB, free PSRAM %u KB, emu stack %u B",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)CONFIG_GPSP_EMU_TASK_STACK_SIZE);

    /* Start emulation on the configured core */
    BaseType_t ret = xTaskCreatePinnedToCore(
        gba_emulation_task,
        "gba_emu",
        CONFIG_GPSP_EMU_TASK_STACK_SIZE,
        NULL,
        configMAX_PRIORITIES - 1,   /* Highest priority */
        NULL,
        CONFIG_GPSP_EMULATION_CORE
    );


    if (ret != pdPASS) {
        ESP_LOGE(TAG,
                 "Failed to create emulation task (largest internal block %u B, free internal %u B, largest PSRAM block %u B, free PSRAM %u B)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return;
    }

    ESP_LOGI(TAG, "Emulation task launched, app_main returning");
    /* app_main returns; FreeRTOS scheduler keeps running the emulation task */
}
