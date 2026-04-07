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

#include "common.h"
#include "cpu.h"
#include "main.h"
#include "storage.h"

static const char *TAG = "gpsp_config";

bool gpsp_netplay_udp_enabled;
uint16_t gpsp_netplay_udp_port;
uint32_t gpsp_netplay_peer_timeout_ms;
uint32_t gpsp_netplay_hello_interval_ms;
int gpsp_netplay_local_client_id_override;

char gpsp_netplay_broadcast_addr[16];

static int parse_bool_value(const char *value, int *out)
{
    if (!value || !out) {
        return 0;
    }

    if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0 ||
        strcasecmp(value, "enabled") == 0) {
        *out = 1;
        return 1;
    }

    if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0 ||
        strcasecmp(value, "disabled") == 0) {
        *out = 0;
        return 1;
    }

    return 0;
}

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
#ifdef HAVE_DYNAREC
    dynarec_enable = 1;
#else
    dynarec_enable = 0;
#endif
    sprite_limit = 1;
    selected_boot_mode = boot_game;

    gpsp_netplay_udp_enabled = CONFIG_GPSP_NETPLAY_UDP_ENABLE;
    gpsp_netplay_udp_port = CONFIG_GPSP_NETPLAY_UDP_PORT;
    gpsp_netplay_peer_timeout_ms = CONFIG_GPSP_NETPLAY_PEER_TIMEOUT_MS;
    gpsp_netplay_hello_interval_ms = CONFIG_GPSP_NETPLAY_HELLO_INTERVAL_MS;
    gpsp_netplay_local_client_id_override = -1;
    strlcpy(gpsp_netplay_broadcast_addr,
            CONFIG_GPSP_NETPLAY_BROADCAST_ADDR,
            sizeof(gpsp_netplay_broadcast_addr));
}

static void gpsp_runtime_config_apply_pair(const char *key, const char *value)
{
    if (strcmp(key, "dynarec_enable") == 0) {
        int enabled;
        if (parse_bool_value(value, &enabled)) {
#ifdef HAVE_DYNAREC
            if (dynarec_enable != enabled) {
                dynarec_enable = enabled;
                flush_dynarec_caches();
            }
#else
            dynarec_enable = 0;
#endif
        } else {
            ESP_LOGW(TAG, "Ignoring invalid bool for %s: %s", key, value);
        }
        return;
    }

    if (strcmp(key, "sprite_limit") == 0) {
        int enabled;
        if (parse_bool_value(value, &enabled)) {
            sprite_limit = enabled;
        } else {
            ESP_LOGW(TAG, "Ignoring invalid bool for %s: %s", key, value);
        }
        return;
    }

    if (strcmp(key, "bios_animation") == 0) {
        int enabled;
        if (parse_bool_value(value, &enabled)) {
            selected_boot_mode = enabled ? boot_bios : boot_game;
        } else {
            ESP_LOGW(TAG, "Ignoring invalid bool for %s: %s", key, value);
        }
        return;
    }

    if (strcmp(key, "boot_mode") == 0) {
        if (strcasecmp(value, "bios") == 0) {
            selected_boot_mode = boot_bios;
        } else if (strcasecmp(value, "game") == 0) {
            selected_boot_mode = boot_game;
        }
        return;
    }

    char *endptr = NULL;
    long parsed_long;

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
            return;
        }
    }

    ESP_LOGW(TAG, "Ignoring unknown or invalid config entry: %s=%s", key, value);
}

esp_err_t gpsp_runtime_config_save(void)
{
    FILE *config_file = fopen(GPSP_RUNTIME_CONFIG_PATH, "w");

    if (!config_file) {
        ESP_LOGW(TAG, "Failed to open config file for write: %s", GPSP_RUNTIME_CONFIG_PATH);
        return ESP_FAIL;
    }

    fprintf(config_file,
            "# gpsp runtime config\n"
            "dynarec_enable=%d\n"
            "sprite_limit=%d\n"
            "bios_animation=%d\n"
            "netplay_udp_enable=%d\n"
            "netplay_udp_port=%u\n"
            "netplay_broadcast_addr=%s\n"
            "netplay_peer_timeout_ms=%u\n"
            "netplay_hello_interval_ms=%u\n"
            "netplay_local_client_id=%d\n",
            dynarec_enable ? 1 : 0,
            sprite_limit ? 1 : 0,
            selected_boot_mode == boot_bios ? 1 : 0,
            gpsp_netplay_udp_enabled ? 1 : 0,
            gpsp_netplay_udp_port,
            gpsp_netplay_broadcast_addr,
            (unsigned)gpsp_netplay_peer_timeout_ms,
            (unsigned)gpsp_netplay_hello_interval_ms,
            gpsp_netplay_local_client_id_override);
    fclose(config_file);
    return ESP_OK;
}

esp_err_t gpsp_runtime_config_init(void)
{
    FILE *config_file;
    char line[160];

    gpsp_runtime_config_set_defaults();

    ESP_LOGI(TAG, "Loading runtime config from %s", GPSP_RUNTIME_CONFIG_PATH);

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
        ESP_LOGI(TAG, "Config line: %s=%s", key, value);
        gpsp_runtime_config_apply_pair(key, value);
    }

    fclose(config_file);
    ESP_LOGI(TAG,
             "Runtime config loaded: dynarec=%d sprite_limit=%d boot=%s",
             dynarec_enable ? 1 : 0,
             sprite_limit ? 1 : 0,
             selected_boot_mode == boot_bios ? "bios" : "game");
    ESP_LOGI(TAG,
             "Runtime netplay: enable=%d port=%u broadcast=%s peer_timeout=%u hello=%u local_client_id=%d",
             gpsp_netplay_udp_enabled ? 1 : 0,
             gpsp_netplay_udp_port,
             gpsp_netplay_broadcast_addr,
             (unsigned)gpsp_netplay_peer_timeout_ms,
             (unsigned)gpsp_netplay_hello_interval_ms,
             gpsp_netplay_local_client_id_override);
    return ESP_OK;
}