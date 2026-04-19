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
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"

/* gpsp core headers */
#include "common.h"
#include "main.h"
#include "gba_memory.h"
#include "sound.h"

extern esp_err_t netpacket_background_start(BaseType_t core_id, UBaseType_t priority);

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
 * Parallel boot
 *
 * Hardware initialization runs on two cores simultaneously:
 *
 *   Path A (emu core):  SD card mount → config file    (~200ms)
 *   Path B (svc core):  LCD panel → audio codec → GPIO (~420ms)
 *
 * Once Path A finishes, ROM/BIOS/save loading begins immediately —
 * overlapping with the slower LCD panel initialization on Path B.
 * The emulation task is created only after both paths and the ROM
 * load have completed.
 *
 * Timeline (typical):
 *
 *   0          200ms               440ms       700ms
 *   ├─ Path A ──┤                    |           |
 *   ├────────── Path B ──────────────┤           |
 *                ├── ROM + BIOS + save load ─────┤
 *                                    ├ av_pipeline┤
 *                                                 ├─ emulation →
 * ================================================================ */

#define BOOT_EVT_STORAGE  BIT0
#define BOOT_EVT_AV       BIT1

typedef struct {
    EventGroupHandle_t evt;
    bool               audio_ready;
    esp_err_t          storage_err;
    esp_err_t          av_err;
} boot_ctx_t;

/* Path A — SD card + config file (runs on emu core) */
static void boot_storage_task(void *arg)
{
    boot_ctx_t *ctx = (boot_ctx_t *)arg;
    int64_t t0 = esp_timer_get_time();

    ctx->storage_err = storage_init();
    if (ctx->storage_err == ESP_OK) {
        esp_err_t cfg = gpsp_runtime_config_init();
        if (cfg != ESP_OK)
            ESP_LOGW(TAG, "Runtime config init failed: %s", esp_err_to_name(cfg));
    } else {
        ESP_LOGE(TAG, "Storage init failed: %s", esp_err_to_name(ctx->storage_err));
    }

    ESP_LOGI(TAG, "Boot path A (storage+config): %lld us",
             (long long)(esp_timer_get_time() - t0));

    xEventGroupSetBits(ctx->evt, BOOT_EVT_STORAGE);
    vTaskDelete(NULL);
}

/* Path B — display + audio + input (runs on service core) */
static void boot_av_task(void *arg)
{
    boot_ctx_t *ctx = (boot_ctx_t *)arg;
    int64_t t0 = esp_timer_get_time();

    /* Video (MIPI-DSI + ST7701 panel) */
    video_driver_config_t vcfg = {
        .lcd_h_res       = CONFIG_GPSP_LCD_H_RES,
        .lcd_v_res       = CONFIG_GPSP_LCD_V_RES,
        .use_ppa_scaling = CONFIG_GPSP_USE_PPA_SCALING,
        .num_fbs         = CONFIG_GPSP_NUM_FB,
    };
    ctx->av_err = video_driver_init(&vcfg);
    if (ctx->av_err != ESP_OK) {
        ESP_LOGE(TAG, "Video init failed: %s", esp_err_to_name(ctx->av_err));
        goto done;
    }
    video_driver_set_brightness(CONFIG_GPSP_LCD_BRIGHTNESS);

    /* Audio (ES8311 codec via I2C + I2S) */
    audio_driver_config_t acfg = { .sample_rate = GPSP_AUDIO_OUTPUT_RATE };
    ESP_LOGI(TAG, "Audio path: gpsp PCM %d Hz -> I2S output %d Hz",
             GPSP_AUDIO_SOURCE_RATE, GPSP_AUDIO_OUTPUT_RATE);
    esp_err_t aerr = audio_driver_init(&acfg);
    if (aerr != ESP_OK) {
        ESP_LOGW(TAG, "Audio init failed: %s (continuing without sound)",
                 esp_err_to_name(aerr));
    } else {
        audio_driver_set_volume(CONFIG_GPSP_AUDIO_VOLUME);
        ctx->audio_ready = true;
    }

    /* Input (GPIO buttons) */
    input_driver_config_t icfg = {
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
    esp_err_t ierr = input_driver_init(&icfg);
    if (ierr != ESP_OK)
        ESP_LOGW(TAG, "Input init failed: %s", esp_err_to_name(ierr));

done:
    ESP_LOGI(TAG, "Boot path B (video+audio+input): %lld us",
             (long long)(esp_timer_get_time() - t0));

    xEventGroupSetBits(ctx->evt, BOOT_EVT_AV);
    vTaskDelete(NULL);
}

/* Resolve which ROM to boot: most-recent game or Kconfig default */
static const char *resolve_boot_rom(void)
{
    static char recent_rom_path[512];
    char **recent_list = NULL;
    size_t recent_count = 0;

    storage_read_recent_list(&recent_list, &recent_count, 10);

    if (recent_count > 0 && recent_list && recent_list[0]) {
        struct stat st;
        if (stat(recent_list[0], &st) == 0 && S_ISREG(st.st_mode)) {
            strlcpy(recent_rom_path, recent_list[0], sizeof(recent_rom_path));
            ESP_LOGI(TAG, "Auto-boot from recent: %s", recent_rom_path);
            for (size_t i = 0; i < recent_count; i++) free(recent_list[i]);
            free(recent_list);
            return recent_rom_path;
        }
        ESP_LOGW(TAG, "Recent ROM not found: %s, using default", recent_list[0]);
    }
    if (recent_list) {
        for (size_t i = 0; i < recent_count; i++) free(recent_list[i]);
        free(recent_list);
    }
    return CONFIG_GPSP_ROM_PATH;
}

/* ================================================================
 * app_main — ESP-IDF entry point
 * ================================================================ */

void app_main(void)
{
    int64_t t_boot = esp_timer_get_time();
    int64_t t0;

    ESP_LOGI(TAG, "===================================");
    ESP_LOGI(TAG, "  gpsp-esp32p4 GBA Emulator");
    ESP_LOGI(TAG, "  Platform: ESP32-P4 @ %d MHz",
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    ESP_LOGI(TAG, "===================================");

    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM: %u MB", (unsigned)(psram_size / (1024 * 1024)));
    if (psram_size < 8 * 1024 * 1024) {
        ESP_LOGE(TAG, "Insufficient PSRAM! Need at least 8 MB");
        return;
    }

    ESP_LOGI(TAG, "Free internal RAM: %u KB  Free PSRAM: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* ──────────────────────────────────────────────────────────────
     * Phase 1 — Launch parallel hardware init
     * ────────────────────────────────────────────────────────────── */
    boot_ctx_t ctx = {
        .evt         = xEventGroupCreate(),
        .audio_ready = false,
        .storage_err = ESP_FAIL,
        .av_err      = ESP_FAIL,
    };
    if (!ctx.evt) { ESP_LOGE(TAG, "No memory for boot events"); return; }

    t0 = esp_timer_get_time();

    xTaskCreatePinnedToCore(boot_storage_task, "boot_sd", 4096, &ctx,
                            configMAX_PRIORITIES - 2, NULL,
                            CONFIG_GPSP_EMULATION_CORE);
    xTaskCreatePinnedToCore(boot_av_task,      "boot_av", 4096, &ctx,
                            configMAX_PRIORITIES - 2, NULL,
                            SERVICE_CORE);

    /* ──────────────────────────────────────────────────────────────
     * Phase 2 — As soon as SD is mounted, start loading the game.
     *           LCD panel init continues in parallel on Path B.
     * ────────────────────────────────────────────────────────────── */
    xEventGroupWaitBits(ctx.evt, BOOT_EVT_STORAGE, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "BOOT storage ready: %lld us",
             (long long)(esp_timer_get_time() - t0));

    if (ctx.storage_err != ESP_OK) {
        ESP_LOGE(TAG, "Storage init failed, halting");
        vEventGroupDelete(ctx.evt);
        return;
    }

    /* Resolve ROM path (needs SD card) */
    char bios_path[64];
    snprintf(bios_path, sizeof(bios_path), "%s/gba_bios.bin", STORAGE_MOUNT_POINT);

    gba_session_boot_config_t session_cfg = {
        .rom_path  = resolve_boot_rom(),
        .bios_path = bios_path,
    };

    ESP_LOGI(TAG, "GBA config: rom=%s bios=%s dynarec=%d audio=?",
             session_cfg.rom_path, session_cfg.bios_path, dynarec_enable);

    /* Load BIOS + ROM + save + reset GBA — no video dependency */
    int64_t t_session = esp_timer_get_time();
    esp_err_t err = gba_session_init(&session_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GBA init failed, halting");
        vEventGroupDelete(ctx.evt);
        return;
    }
    ESP_LOGI(TAG, "BOOT session_init: %lld us",
             (long long)(esp_timer_get_time() - t_session));

    /* ──────────────────────────────────────────────────────────────
     * Phase 3 — Wait for AV hardware, then wire up the pipeline
     *           and start emulation.
     * ────────────────────────────────────────────────────────────── */
    xEventGroupWaitBits(ctx.evt, BOOT_EVT_AV, pdFALSE, pdTRUE, portMAX_DELAY);
    vEventGroupDelete(ctx.evt);

    ESP_LOGI(TAG, "BOOT platform total: %lld us",
             (long long)(esp_timer_get_time() - t0));

    if (ctx.av_err != ESP_OK) {
        ESP_LOGE(TAG, "AV init failed, halting");
        return;
    }

    /* AV pipeline (needs both video and audio to be ready) */
    {
        av_pipeline_config_t av_cfg = { .audio_enabled = ctx.audio_ready };
        err = av_pipeline_init(&av_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AV pipeline init failed, halting");
            return;
        }
    }

    /* Async services */
    err = c6_remote_start_task(SERVICE_CORE, configMAX_PRIORITIES - 3);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C6 remote task failed: %s", esp_err_to_name(err));
        return;
    }
    err = netpacket_background_start(SERVICE_CORE, configMAX_PRIORITIES - 4);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Netpacket task failed: %s", esp_err_to_name(err));
        return;
    }
    if (gpsp_web_server_enabled)
        web_server_start_async(SERVICE_CORE);

    /* Start emulation */
    ESP_LOGI(TAG, "Free internal RAM %u KB, free PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    BaseType_t ret = xTaskCreatePinnedToCore(
        gba_emulation_task, "gba_emu",
        CONFIG_GPSP_EMU_TASK_STACK_SIZE, NULL,
        configMAX_PRIORITIES - 1,
        NULL, CONFIG_GPSP_EMULATION_CORE);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create emulation task");
        return;
    }

    ESP_LOGI(TAG, "BOOT total: %lld us — emulation started",
             (long long)(esp_timer_get_time() - t_boot));
}
