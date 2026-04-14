/*
 * gpsp app support — ESP32-C6 remote link bring-up
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "c6_remote.h"
#include "common.h"

#if CONFIG_GPSP_ENABLE_C6_REMOTE

#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define C6_WIFI_CONNECTED_BIT BIT0
#define C6_WIFI_FAILED_BIT    BIT1
#define C6_REMOTE_TASK_STACK_SIZE 8192

static const char *TAG = "gpsp_c6_remote";

static EventGroupHandle_t c6_wifi_event_group;
static esp_netif_t *c6_wifi_sta;
static esp_event_handler_instance_t c6_wifi_any_id;
static esp_event_handler_instance_t c6_ip_got_ip;
static int c6_wifi_retry_count;
static bool c6_transport_ready;
static bool c6_wifi_connected;

static void c6_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        c6_wifi_connected = false;
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
        c6_wifi_connected = true;
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

    c6_transport_ready = true;
    c6_wifi_connected = false;

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

    return ESP_OK;
}

static void c6_remote_task(void *param)
{
    (void)param;

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
        if (esp_wifi_sta_get_ap_info(&current_ap) != ESP_OK) {
            ESP_LOGI(TAG, "ESP32-C6 remote link alive, STA not associated");
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(CONFIG_GPSP_C6_HEALTH_LOG_PERIOD_MS));
    }
}

esp_err_t c6_remote_start_task(BaseType_t core_id, UBaseType_t priority)
{
    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(
        c6_remote_task,
        "c6_remote",
        C6_REMOTE_TASK_STACK_SIZE,
        NULL,
        priority,
        NULL,
        core_id,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (task_ret != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool c6_remote_network_ready(void)
{
    return c6_transport_ready && c6_wifi_connected;
}

#else

esp_err_t c6_remote_start_task(BaseType_t core_id, UBaseType_t priority)
{
    (void)core_id;
    (void)priority;
    return ESP_OK;
}

bool c6_remote_network_ready(void)
{
    return false;
}

#endif