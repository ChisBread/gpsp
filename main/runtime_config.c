/*
 * gpsp app support — runtime configuration loaded from SD card
 */

#include "runtime_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"

#include "storage.h"

static const char *TAG = "gpsp_config";

#ifdef CONFIG_GPSP_NETPLAY_UDP_ENABLE
bool gpsp_netplay_udp_enabled;
uint16_t gpsp_netplay_udp_port;
uint32_t gpsp_netplay_peer_timeout_ms;
uint32_t gpsp_netplay_hello_interval_ms;
int gpsp_netplay_local_client_id_override;
#endif

char gpsp_netplay_broadcast_addr[16];

static char *trim_whitespace(char *text)
{
    char *end;

    while (*text && isspace((unsigned char)*text)) {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }

    return text;
}

static void gpsp_runtime_config_set_defaults(void)
{
#ifdef CONFIG_GPSP_NETPLAY_UDP_ENABLE
    gpsp_netplay_udp_enabled = CONFIG_GPSP_NETPLAY_UDP_ENABLE;
    gpsp_netplay_udp_port = CONFIG_GPSP_NETPLAY_UDP_PORT;
    gpsp_netplay_peer_timeout_ms = CONFIG_GPSP_NETPLAY_PEER_TIMEOUT_MS;
    gpsp_netplay_hello_interval_ms = CONFIG_GPSP_NETPLAY_HELLO_INTERVAL_MS;
    gpsp_netplay_local_client_id_override = -1;
    strlcpy(gpsp_netplay_broadcast_addr,
            CONFIG_GPSP_NETPLAY_BROADCAST_ADDR,
            sizeof(gpsp_netplay_broadcast_addr));
#endif
}

static void gpsp_runtime_config_apply_pair(const char *key, const char *value)
{
    char *endptr = NULL;
    long parsed_long;
#ifdef CONFIG_GPSP_NETPLAY_UDP_ENABLE
    if (strcmp(key, "netplay_udp_enable") == 0) {
        gpsp_netplay_udp_enabled = atoi(value) != 0;
        return;
    }

    if (strcmp(key, "netplay_udp_port") == 0) {
        parsed_long = strtol(value, &endptr, 10);
        if (endptr != value && parsed_long >= 1024 && parsed_long <= 65535) {
            gpsp_netplay_udp_port = (uint16_t)parsed_long;
        }
        return;
    }

    if (strcmp(key, "netplay_broadcast_addr") == 0) {
        strlcpy(gpsp_netplay_broadcast_addr, value, sizeof(gpsp_netplay_broadcast_addr));
        return;
    }

    if (strcmp(key, "netplay_peer_timeout_ms") == 0) {
        parsed_long = strtol(value, &endptr, 10);
        if (endptr != value && parsed_long >= 100 && parsed_long <= 30000) {
            gpsp_netplay_peer_timeout_ms = (uint32_t)parsed_long;
        }
        return;
    }

    if (strcmp(key, "netplay_hello_interval_ms") == 0) {
        parsed_long = strtol(value, &endptr, 10);
        if (endptr != value && parsed_long >= 50 && parsed_long <= 5000) {
            gpsp_netplay_hello_interval_ms = (uint32_t)parsed_long;
        }
        return;
    }

    if (strcmp(key, "netplay_local_client_id") == 0) {
        parsed_long = strtol(value, &endptr, 10);
        if (endptr != value && parsed_long >= -1 && parsed_long <= 31) {
            gpsp_netplay_local_client_id_override = (int)parsed_long;
        }
    }
#endif
}

esp_err_t gpsp_runtime_config_save(void)
{
    FILE *config_file = fopen(GPSP_RUNTIME_CONFIG_PATH, "w");

    if (!config_file) {
        ESP_LOGW(TAG, "Failed to open config file for write: %s", GPSP_RUNTIME_CONFIG_PATH);
        return ESP_FAIL;
    }
#ifdef CONFIG_GPSP_NETPLAY_UDP_ENABLE
    fprintf(config_file,
            "# gpsp runtime config\n"
            "netplay_udp_enable=%d\n"
            "netplay_udp_port=%u\n"
            "netplay_broadcast_addr=%s\n"
            "netplay_peer_timeout_ms=%u\n"
            "netplay_hello_interval_ms=%u\n"
            "netplay_local_client_id=%d\n",
            gpsp_netplay_udp_enabled ? 1 : 0,
            gpsp_netplay_udp_port,
            gpsp_netplay_broadcast_addr,
            (unsigned)gpsp_netplay_peer_timeout_ms,
            (unsigned)gpsp_netplay_hello_interval_ms,
            gpsp_netplay_local_client_id_override);
#endif
    fclose(config_file);
    return ESP_OK;
}

esp_err_t gpsp_runtime_config_init(void)
{
    FILE *config_file;
    char line[160];

    gpsp_runtime_config_set_defaults();

    config_file = fopen(GPSP_RUNTIME_CONFIG_PATH, "r");
    if (!config_file) {
        ESP_LOGI(TAG, "Config file not found, writing defaults to %s", GPSP_RUNTIME_CONFIG_PATH);
        return gpsp_runtime_config_save();
    }

    while (fgets(line, sizeof(line), config_file)) {
        char *separator;
        char *key;
        char *value;

        key = trim_whitespace(line);
        if (*key == '\0' || *key == '#') {
            continue;
        }

        separator = strchr(key, '=');
        if (!separator) {
            continue;
        }

        *separator = '\0';
        value = trim_whitespace(separator + 1);
        key = trim_whitespace(key);
        gpsp_runtime_config_apply_pair(key, value);
    }

    fclose(config_file);
#ifdef CONFIG_GPSP_NETPLAY_UDP_ENABLE

    ESP_LOGI(TAG,
             "Runtime config loaded: netplay=%d port=%u broadcast=%s peer_timeout=%u hello=%u local_client_id=%d",
             gpsp_netplay_udp_enabled ? 1 : 0,
             gpsp_netplay_udp_port,
             gpsp_netplay_broadcast_addr,
             (unsigned)gpsp_netplay_peer_timeout_ms,
             (unsigned)gpsp_netplay_hello_interval_ms,
             gpsp_netplay_local_client_id_override);
#endif
    return ESP_OK;
}