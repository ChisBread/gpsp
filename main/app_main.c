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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_psram.h"

/* gpsp core headers */
#include "common.h"
#include "main.h"
#include "cpu.h"
#include "gba_memory.h"
#include "sound.h"
#include "video.h"

/* ESP32-P4 platform drivers (JC4880) */
#include "video_driver.h"
#include "audio_driver.h"
#include "input_driver.h"
#include "storage.h"

static const char *TAG = "gpsp_main";

/* ---- Globals expected by gpsp core ---- */
u32 skip_next_frame = 0;
int dynarec_enable = 0;     /* Start with interpreter mode */
int sprite_limit = 1;
boot_mode selected_boot_mode = boot_game;

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;

/* Netplay stubs — gpsp serial/rfu code references these libretro symbols.
 * On ESP32-P4 we don't use libretro netplay; provide no-op implementations.
 * Future: could implement GBA link cable over WiFi via ESP32-C6. */
u32 netplay_num_clients = 0;
u32 netplay_client_id = 0;
void netpacket_poll_receive(void) { }
void netpacket_send(uint16_t client_id, const void *buf, size_t len) { (void)client_id; (void)buf; (void)len; }

/* Framebuffer pointer used by gpsp video.cc */
static GPSP_EXTRAM_BSS u16 gba_screen_buffer[GBA_SCREEN_WIDTH * (GBA_SCREEN_HEIGHT + 1)] __attribute__((aligned(64)));

/* Audio buffer for outputting to I2S */
#define AUDIO_SAMPLES_PER_FRAME  (CONFIG_GPSP_AUDIO_SAMPLE_RATE / 60)

/* ---- Performance counters ---- */
static int64_t frame_start_us = 0;
static uint32_t fps_counter = 0;
static int64_t fps_timer_us = 0;

/* ================================================================
 * Platform initialization
 * ================================================================ */

static esp_err_t init_platform(void)
{
    esp_err_t err;

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
        .sample_rate = CONFIG_GPSP_AUDIO_SAMPLE_RATE,
    };
    err = audio_driver_init(&audio_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Audio init failed: %s (continuing without sound)",
                 esp_err_to_name(err));
        /* Audio failure is non-fatal */
    } else {
        audio_driver_set_volume(CONFIG_GPSP_AUDIO_VOLUME);
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
 * ROM loading and GBA initialization
 * ================================================================ */

static esp_err_t load_and_init_gba(const char *rom_path)
{
    /* Initialize gpsp core systems */
    init_main();
    init_gamepak_buffer();
    init_sound();

    /* Set up screen pixel buffer */
    if (!gba_screen_pixels) {
        gba_screen_pixels = gba_screen_buffer;
    }

    /* Load BIOS (try from SD card, fall back to built-in) */
    bool bios_loaded = false;
    char bios_path[64];
    snprintf(bios_path, sizeof(bios_path), "%s/gba_bios.bin", STORAGE_MOUNT_POINT);
    if (load_bios(bios_path) == 0 && bios_rom[0] == 0x18) {
        ESP_LOGI(TAG, "Official BIOS loaded from SD");
        bios_loaded = true;
    }
    if (!bios_loaded) {
        memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
        ESP_LOGI(TAG, "Using built-in HLE BIOS");
    }

    /* Load ROM via gpsp core (uses libretro VFS → standard fopen) */
    memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
    if (load_gamepak(NULL, rom_path, 0, 0, 0) != 0) {
        ESP_LOGE(TAG, "Failed to load ROM: %s", rom_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ROM loaded: %s (%u bytes)", rom_path, (unsigned)gamepak_size);

    /* Initialize CPU state */
    reset_gba();

    ESP_LOGI(TAG, "GBA system initialized, ready to run");
    return ESP_OK;
}

/* ================================================================
 * Main emulation loop
 * ================================================================ */

static void emulation_task(void *param)
{
    ESP_LOGI(TAG, "Emulation task started on core %d", xPortGetCoreID());

    int remaining_cycles = 0;
    int16_t audio_buf[AUDIO_SAMPLES_PER_FRAME * 2];  /* stereo */

    fps_timer_us = esp_timer_get_time();

    while (1) {
        frame_start_us = esp_timer_get_time();

        /* ---- Poll input ---- */
        uint16_t keys = input_driver_read();
        /* Write to GBA KEYINPUT register (active-low: 0 = pressed) */
        write_ioreg(REG_P1, ~keys & 0x3FF);

        /* ---- Run one frame of GBA CPU ---- */
        u32 result;
        do {
            result = update_gba(remaining_cycles);
            remaining_cycles = cycles_to_run(result);
        } while (!completed_frame(result));

        /* ---- Submit video frame ---- */
        if (!skip_next_frame) {
            video_driver_submit_frame(gba_screen_pixels);
        }

        /* ---- Output audio ---- */
        /* TODO: Extract rendered audio samples from gpsp sound buffer
         * and write to I2S via audio_driver_write() */

        /* ---- FPS counter ---- */
        fps_counter++;
        int64_t now = esp_timer_get_time();
        if (now - fps_timer_us >= 1000000) {
            ESP_LOGI(TAG, "FPS: %u | Free heap: %u KB",
                     (unsigned)fps_counter,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
            fps_counter = 0;
            fps_timer_us = now;
        }

        /* ---- Frame pacing (target ~16.74ms per frame = 59.73 Hz) ---- */
        int64_t elapsed_us = esp_timer_get_time() - frame_start_us;
        int64_t target_us = 16742;  /* 1000000 / 59.7275 */
        if (elapsed_us < target_us) {
            vTaskDelay(pdMS_TO_TICKS((target_us - elapsed_us) / 1000));
        }
    }
}

/* ================================================================
 * app_main — ESP-IDF entry point
 * ================================================================ */

void app_main(void)
{
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
    esp_err_t err = init_platform();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Platform init failed, halting");
        return;
    }

    /* Load ROM and initialize GBA */
    err = load_and_init_gba(CONFIG_GPSP_ROM_PATH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GBA init failed, halting");
        return;
    }

    /* Start emulation on the configured core */
    BaseType_t ret = xTaskCreatePinnedToCore(
        emulation_task,
        "gba_emu",
        CONFIG_GPSP_EMU_TASK_STACK_SIZE,
        NULL,
        configMAX_PRIORITIES - 1,   /* Highest priority */
        NULL,
        CONFIG_GPSP_EMULATION_CORE
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create emulation task");
        return;
    }

    ESP_LOGI(TAG, "Emulation task launched, app_main returning");
    /* app_main returns; FreeRTOS scheduler keeps running the emulation task */
}
