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

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#include "esp_timer.h"
#include "esp_psram.h"
#include "nvs_flash.h"

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

/* Audio buffer for outputting to I2S */
#define GBA_FRAME_RATE           59.7275f
#define GPSP_AUDIO_OUTPUT_RATE   GBA_SOUND_FREQUENCY
#define AUDIO_FRAME_SAMPLES_MAX  ((GPSP_AUDIO_OUTPUT_RATE / 50) + 1)
#define AV_PIPELINE_DEPTH        2
#define AV_TASK_STACK_SIZE       8192
#define AV_OUTPUT_CORE           ((CONFIG_GPSP_EMULATION_CORE == 0) ? 1 : 0)

/* ---- Performance counters ---- */
static int64_t frame_start_us = 0;
static uint32_t fps_counter = 0;
static int64_t fps_timer_us = 0;
static bool audio_enabled = false;
static float audio_frame_samples = 0.0f;
static float audio_frame_fraction = 0.0f;

/* AV pipeline buffers: emulation produces on one core, platform output consumes on the other. */
static GPSP_EXTRAM_BSS u16 gba_framebuffers[AV_PIPELINE_DEPTH][GBA_SCREEN_WIDTH * (GBA_SCREEN_HEIGHT + 1)] __attribute__((aligned(64)));
static int16_t audio_buffers[AV_PIPELINE_DEPTH][AUDIO_FRAME_SAMPLES_MAX * 2];
static uint32_t audio_buffer_frames[AV_PIPELINE_DEPTH];
static bool skip_video_submit[AV_PIPELINE_DEPTH];
static QueueHandle_t av_free_queue = NULL;
static QueueHandle_t av_ready_queue = NULL;

#if CONFIG_GPSP_ENABLE_C6_REMOTE
#define C6_WIFI_CONNECTED_BIT BIT0
#define C6_WIFI_FAILED_BIT    BIT1

static bool c6_remote_ready = false;
static EventGroupHandle_t c6_wifi_event_group = NULL;
static esp_netif_t *c6_wifi_sta = NULL;
static esp_event_handler_instance_t c6_wifi_any_id = NULL;
static esp_event_handler_instance_t c6_ip_got_ip = NULL;
static int c6_wifi_retry_count = 0;

static void c6_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
#if CONFIG_GPSP_C6_CONNECT_TEST
        if (c6_wifi_retry_count < CONFIG_GPSP_C6_CONNECT_MAX_RETRIES) {
            c6_wifi_retry_count++;
            ESP_LOGW(TAG, "Remote STA disconnected, retry %d/%d",
                     c6_wifi_retry_count, CONFIG_GPSP_C6_CONNECT_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(c6_wifi_event_group, C6_WIFI_FAILED_BIT);
        }
#else
        (void)event_data;
#endif
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "ESP32-C6 remote STA got IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));
        c6_wifi_retry_count = 0;
        xEventGroupSetBits(c6_wifi_event_group, C6_WIFI_CONNECTED_BIT);
    }
}

static esp_err_t init_c6_remote_transport(void)
{
    esp_err_t err;
    esp_hosted_coprocessor_fwver_t fwver = {0};
    esp_hosted_app_desc_t app_desc = {0};

    err = esp_hosted_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_hosted_connect_to_slave();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_hosted_get_coprocessor_fwversion(&fwver);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "ESP32-C6 hosted FW version: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
             fwver.major1, fwver.minor1, fwver.patch1);

    err = esp_hosted_get_coprocessor_app_desc(&app_desc);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ESP32-C6 app: %s %s (%s %s)",
                 app_desc.project_name,
                 app_desc.version,
                 app_desc.date,
                 app_desc.time);
    } else {
        ESP_LOGW(TAG, "Failed to read ESP32-C6 app description: %s",
                 esp_err_to_name(err));
    }

    return ESP_OK;
}

static esp_err_t init_c6_remote_wifi(void)
{
    esp_err_t err;
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (!c6_wifi_event_group) {
        c6_wifi_event_group = xEventGroupCreate();
        if (!c6_wifi_event_group) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!c6_wifi_any_id) {
        err = esp_event_handler_instance_register(WIFI_EVENT,
                                                  ESP_EVENT_ANY_ID,
                                                  &c6_wifi_event_handler,
                                                  NULL,
                                                  &c6_wifi_any_id);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (!c6_ip_got_ip) {
        err = esp_event_handler_instance_register(IP_EVENT,
                                                  IP_EVENT_STA_GOT_IP,
                                                  &c6_wifi_event_handler,
                                                  NULL,
                                                  &c6_ip_got_ip);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (!c6_wifi_sta) {
        c6_wifi_sta = esp_netif_create_default_wifi_sta();
        if (!c6_wifi_sta) {
            return ESP_FAIL;
        }
    }

    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    return ESP_OK;
}

static esp_err_t c6_remote_scan_wifi(void)
{
    esp_err_t err;
    uint16_t ap_count = 0;
    uint16_t record_count = CONFIG_GPSP_C6_SCAN_LIST_SIZE;
    wifi_ap_record_t *ap_records = NULL;

    ap_records = calloc(record_count, sizeof(*ap_records));
    if (!ap_records) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        free(ap_records);
        return err;
    }

    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        free(ap_records);
        return err;
    }

    err = esp_wifi_scan_get_ap_records(&record_count, ap_records);
    if (err != ESP_OK) {
        free(ap_records);
        return err;
    }

    ESP_LOGI(TAG, "ESP32-C6 remote scan found %u APs, logging %u",
             ap_count, record_count);
    for (uint16_t index = 0; index < record_count; index++) {
        ESP_LOGI(TAG, "AP[%u] ssid=%s rssi=%d channel=%u auth=%d",
                 index,
                 (const char *)ap_records[index].ssid,
                 ap_records[index].rssi,
                 ap_records[index].primary,
                 ap_records[index].authmode);
    }

    free(ap_records);
    return ESP_OK;
}

#if CONFIG_GPSP_C6_CONNECT_TEST
static esp_err_t c6_remote_connect_wifi(void)
{
    wifi_config_t wifi_config = { 0 };
    EventBits_t bits;

    if (CONFIG_GPSP_C6_CONNECT_SSID[0] == '\0') {
        ESP_LOGE(TAG, "Hosted STA connect test enabled but SSID is empty");
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy((char *)wifi_config.sta.ssid,
            CONFIG_GPSP_C6_CONNECT_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password,
            CONFIG_GPSP_C6_CONNECT_PASSWORD,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.failure_retry_cnt = CONFIG_GPSP_C6_CONNECT_MAX_RETRIES;

    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    c6_wifi_retry_count = 0;
    xEventGroupClearBits(c6_wifi_event_group, C6_WIFI_CONNECTED_BIT | C6_WIFI_FAILED_BIT);

    ESP_ERROR_CHECK(esp_wifi_connect());

    bits = xEventGroupWaitBits(c6_wifi_event_group,
                               C6_WIFI_CONNECTED_BIT | C6_WIFI_FAILED_BIT,
                               pdTRUE,
                               pdFALSE,
                               pdMS_TO_TICKS(CONFIG_GPSP_C6_CONNECT_TIMEOUT_MS));

    if (bits & C6_WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "ESP32-C6 remote STA connected to SSID '%s'",
                 CONFIG_GPSP_C6_CONNECT_SSID);
        return ESP_OK;
    }

    if (bits & C6_WIFI_FAILED_BIT) {
        ESP_LOGE(TAG, "ESP32-C6 remote STA failed to connect to SSID '%s'",
                 CONFIG_GPSP_C6_CONNECT_SSID);
        return ESP_FAIL;
    }

    ESP_LOGE(TAG, "ESP32-C6 remote STA connect timed out after %d ms",
             CONFIG_GPSP_C6_CONNECT_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}
#endif

static esp_err_t init_c6_remote_link(void)
{
    esp_err_t err;
    uint8_t sta_mac[6] = {0};

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    err = init_c6_remote_transport();
    if (err != ESP_OK) {
        return err;
    }

    err = init_c6_remote_wifi();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_get_mac(WIFI_IF_STA, sta_mac);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG,
             "ESP32-C6 remote link ready, STA MAC %02X:%02X:%02X:%02X:%02X:%02X",
             sta_mac[0], sta_mac[1], sta_mac[2],
             sta_mac[3], sta_mac[4], sta_mac[5]);

    err = c6_remote_scan_wifi();
    if (err != ESP_OK) {
        return err;
    }

#if CONFIG_GPSP_C6_CONNECT_TEST
    err = c6_remote_connect_wifi();
    if (err != ESP_OK) {
        return err;
    }
#endif

    c6_remote_ready = true;
    return ESP_OK;
}

static void c6_remote_task(void *param)
{
    ESP_LOGI(TAG, "C6 remote task started on core %d", xPortGetCoreID());

    esp_err_t err = init_c6_remote_link();
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "ESP32-C6 remote link init failed: %s. Check esp_wifi_remote backend and P4<->C6 transport wiring.",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    while (1) {
#if CONFIG_GPSP_C6_CONNECT_TEST
        wifi_ap_record_t current_ap = {0};
        if (esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK) {
            ESP_LOGI(TAG, "ESP32-C6 remote link alive, connected to '%s' RSSI %d",
                     (const char *)current_ap.ssid,
                     current_ap.rssi);
        } else {
            ESP_LOGI(TAG, "ESP32-C6 remote link alive, STA not associated");
        }
#else
        ESP_LOGI(TAG, "ESP32-C6 remote link alive");
#endif
        vTaskDelay(pdMS_TO_TICKS(CONFIG_GPSP_C6_HEALTH_LOG_PERIOD_MS));
    }
}
#endif

static uint32_t collect_audio_frame(int16_t *audio_buf, size_t audio_buf_frames)
{
    uint32_t frames_to_read;
    uint32_t frames_produced;

    if (!audio_enabled) {
        return 0;
    }

    frames_to_read = (uint32_t)audio_frame_samples;
    audio_frame_fraction += audio_frame_samples - (float)frames_to_read;
    if (audio_frame_fraction >= 1.0f) {
        frames_to_read++;
        audio_frame_fraction -= 1.0f;
    }

    if (frames_to_read > audio_buf_frames) {
        frames_to_read = (uint32_t)audio_buf_frames;
    }

    frames_produced = sound_read_samples(audio_buf, frames_to_read);
    if (frames_produced < frames_to_read) {
        memset(audio_buf + (frames_produced * 2), 0,
               (frames_to_read - frames_produced) * 2 * sizeof(*audio_buf));
    }

    return frames_to_read;
}

static esp_err_t init_av_pipeline(void)
{
    uint32_t slot_index;

    av_free_queue = xQueueCreate(AV_PIPELINE_DEPTH, sizeof(slot_index));
    av_ready_queue = xQueueCreate(AV_PIPELINE_DEPTH, sizeof(slot_index));
    if (!av_free_queue || !av_ready_queue) {
        return ESP_ERR_NO_MEM;
    }

    for (slot_index = 0; slot_index < AV_PIPELINE_DEPTH; slot_index++) {
        audio_buffer_frames[slot_index] = 0;
        skip_video_submit[slot_index] = false;
        xQueueSend(av_free_queue, &slot_index, 0);
    }

    gba_screen_pixels = gba_framebuffers[0];
    return ESP_OK;
}

static void av_output_task(void *param)
{
    ESP_LOGI(TAG, "AV output task started on core %d", xPortGetCoreID());

    while (1) {
        uint32_t slot_index;

        if (xQueueReceive(av_ready_queue, &slot_index, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!skip_video_submit[slot_index]) {
            esp_err_t err = video_driver_submit_frame(gba_framebuffers[slot_index]);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Video submit failed: %s", esp_err_to_name(err));
            }
        }

        if (audio_enabled && audio_buffer_frames[slot_index] > 0) {
            esp_err_t err = audio_driver_write(audio_buffers[slot_index],
                                               audio_buffer_frames[slot_index]);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Audio write failed: %s", esp_err_to_name(err));
                audio_enabled = false;
            }
        }

        xQueueSend(av_free_queue, &slot_index, portMAX_DELAY);
    }
}

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
        .sample_rate = GPSP_AUDIO_OUTPUT_RATE,
    };
    if (CONFIG_GPSP_AUDIO_SAMPLE_RATE != GPSP_AUDIO_OUTPUT_RATE) {
        ESP_LOGW(TAG,
                 "CONFIG_GPSP_AUDIO_SAMPLE_RATE=%d ignored; gpsp core currently outputs %d Hz PCM",
                 CONFIG_GPSP_AUDIO_SAMPLE_RATE, GPSP_AUDIO_OUTPUT_RATE);
    }
    err = audio_driver_init(&audio_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Audio init failed: %s (continuing without sound)",
                 esp_err_to_name(err));
        audio_enabled = false;
        /* Audio failure is non-fatal */
    } else {
        audio_driver_set_volume(CONFIG_GPSP_AUDIO_VOLUME);
        audio_enabled = true;
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
    init_sound();

    /* Set up screen pixel buffer */
    if (!gba_screen_pixels) {
        gba_screen_pixels = gba_framebuffers[0];
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

    /* Allocate ROM buffers last — this consumes all remaining PSRAM.
     * init_gamepak_buffer() gracefully stops when PSRAM is exhausted. */
    u32 rom_buf_count = init_gamepak_buffer();
    ESP_LOGI(TAG, "ROM buffers: %u MB in PSRAM", (unsigned)rom_buf_count);

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

    uint32_t slot_index;

    fps_timer_us = esp_timer_get_time();
    audio_frame_samples = (float)GPSP_AUDIO_OUTPUT_RATE / GBA_FRAME_RATE;
    audio_frame_fraction = 0.0f;

    if (xQueueReceive(av_free_queue, &slot_index, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire initial AV slot");
        vTaskDelete(NULL);
        return;
    }
    gba_screen_pixels = gba_framebuffers[slot_index];

    while (1) {
        frame_start_us = esp_timer_get_time();

        /* ---- Poll input ---- */
        uint16_t keys = input_driver_read();
        /* Write to GBA KEYINPUT register (active-low: 0 = pressed) */
        write_ioreg(REG_P1, ~keys & 0x3FF);
        skip_next_frame = 0;

        /* ---- Run one frame of GBA CPU ---- */
    #ifdef HAVE_DYNAREC
        if (dynarec_enable) {
            execute_arm_translate(execute_cycles);
        } else
    #endif
        {
            clear_gamepak_stickybits();
            execute_arm(execute_cycles);
        }

        /* ---- Hand off frame/audio to the AV task on the other core ---- */
        skip_video_submit[slot_index] = (skip_next_frame != 0);
        audio_buffer_frames[slot_index] = collect_audio_frame(audio_buffers[slot_index],
                                                              AUDIO_FRAME_SAMPLES_MAX);
        xQueueSend(av_ready_queue, &slot_index, portMAX_DELAY);

        if (xQueueReceive(av_free_queue, &slot_index, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to acquire AV slot");
            vTaskDelete(NULL);
            return;
        }
        gba_screen_pixels = gba_framebuffers[slot_index];

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
        if (!audio_enabled) {
            int64_t elapsed_us = esp_timer_get_time() - frame_start_us;
            int64_t target_us = 16742;  /* 1000000 / 59.7275 */
            if (elapsed_us < target_us) {
                vTaskDelay(pdMS_TO_TICKS((target_us - elapsed_us) / 1000));
            }
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

    err = init_av_pipeline();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AV pipeline init failed, halting");
        return;
    }

    /* Load ROM and initialize GBA */
    err = load_and_init_gba(CONFIG_GPSP_ROM_PATH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GBA init failed, halting");
        return;
    }

    BaseType_t av_ret = xTaskCreatePinnedToCore(
        av_output_task,
        "gba_av",
        AV_TASK_STACK_SIZE,
        NULL,
        configMAX_PRIORITIES - 2,
        NULL,
        AV_OUTPUT_CORE
    );

    if (av_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AV output task");
        return;
    }

#if CONFIG_GPSP_ENABLE_C6_REMOTE
    BaseType_t c6_ret = xTaskCreatePinnedToCore(
        c6_remote_task,
        "c6_remote",
        8192,
        NULL,
        configMAX_PRIORITIES - 3,
        NULL,
        AV_OUTPUT_CORE
    );

    if (c6_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create C6 remote task");
        return;
    }
#endif

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
