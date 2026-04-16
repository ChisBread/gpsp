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
#include "gba_memory.h"
#include "main.h"
#include "serial.h"
#include "storage.h"

static const char *TAG = "gpsp_config";

int  gpsp_netplay_ra_mode;
bool gpsp_netplay_ra_enabled;
char gpsp_netplay_ra_host[64];
uint16_t gpsp_netplay_ra_port;
char gpsp_netplay_ra_nick[32];
char gpsp_netplay_ra_tunnel_id[25];
char gpsp_netplay_lobby_host[64];
uint16_t gpsp_netplay_lobby_port;
char gpsp_netplay_lobby_relay[32];

int gpsp_serial_setting;
int gpsp_rtc_mode;

bool gpsp_web_server_enabled;

uint32_t gpsp_frameskip_type;
uint32_t gpsp_frameskip_interval;
uint32_t gpsp_frameskip_threshold;

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

    gpsp_netplay_ra_mode = NETPLAY_MODE_DISABLED;
    gpsp_netplay_ra_enabled = false;
    gpsp_netplay_ra_host[0] = '\0';
    gpsp_netplay_ra_port = 55435;
    strlcpy(gpsp_netplay_ra_nick, "ESP32-P4", sizeof(gpsp_netplay_ra_nick));
    gpsp_netplay_ra_tunnel_id[0] = '\0';
    gpsp_netplay_lobby_host[0] = '\0';
    gpsp_netplay_lobby_port = 7777;
    gpsp_netplay_lobby_relay[0] = '\0';

    gpsp_serial_setting = SERIAL_MODE_AUTO;
    gpsp_rtc_mode = FEAT_AUTODETECT;

    gpsp_web_server_enabled = true;

    gpsp_frameskip_type = 0;
    gpsp_frameskip_interval = 0;
    gpsp_frameskip_threshold = 33;
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

    if (strcmp(key, "netplay_ra_enable") == 0) {
        int enabled;
        if (parse_bool_value(value, &enabled)) {
            gpsp_netplay_ra_enabled = enabled;
            /* Legacy: if enabled but mode is disabled, default to client */
            if (enabled && gpsp_netplay_ra_mode == NETPLAY_MODE_DISABLED)
                gpsp_netplay_ra_mode = NETPLAY_MODE_CLIENT;
        }
        return;
    }

    if (strcmp(key, "netplay_ra_mode") == 0) {
        parsed_long = strtol(value, &endptr, 10);
        if (endptr != value && parsed_long >= 0 && parsed_long <= 4) {
            gpsp_netplay_ra_mode = (int)parsed_long;
            gpsp_netplay_ra_enabled = (gpsp_netplay_ra_mode != NETPLAY_MODE_DISABLED);
        }
        return;
    }

    if (strcmp(key, "netplay_ra_host") == 0) {
        strlcpy(gpsp_netplay_ra_host, value, sizeof(gpsp_netplay_ra_host));
        return;
    }

    if (strcmp(key, "netplay_ra_port") == 0) {
        parsed_long = strtol(value, &endptr, 10);
        if (endptr != value && parsed_long >= 1 && parsed_long <= 65535) {
            gpsp_netplay_ra_port = (uint16_t)parsed_long;
        }
        return;
    }

    if (strcmp(key, "netplay_ra_nick") == 0) {
        strlcpy(gpsp_netplay_ra_nick, value, sizeof(gpsp_netplay_ra_nick));
        return;
    }

    if (strcmp(key, "netplay_ra_tunnel_id") == 0) {
        strlcpy(gpsp_netplay_ra_tunnel_id, value, sizeof(gpsp_netplay_ra_tunnel_id));
        return;
    }

    if (strcmp(key, "netplay_lobby_host") == 0) {
        strlcpy(gpsp_netplay_lobby_host, value, sizeof(gpsp_netplay_lobby_host));
        return;
    }

    if (strcmp(key, "netplay_lobby_port") == 0) {
        parsed_long = strtol(value, &endptr, 10);
        if (endptr != value && parsed_long >= 1 && parsed_long <= 65535) {
            gpsp_netplay_lobby_port = (uint16_t)parsed_long;
        }
        return;
    }

    if (strcmp(key, "netplay_lobby_relay") == 0) {
        strlcpy(gpsp_netplay_lobby_relay, value, sizeof(gpsp_netplay_lobby_relay));
        return;
    }

    if (strcmp(key, "serial_mode") == 0) {
        if (strcasecmp(value, "auto") == 0)
            gpsp_serial_setting = SERIAL_MODE_AUTO;
        else if (strcasecmp(value, "disabled") == 0)
            gpsp_serial_setting = SERIAL_MODE_DISABLED;
        else if (strcasecmp(value, "rfu") == 0)
            gpsp_serial_setting = SERIAL_MODE_RFU;
        else if (strcasecmp(value, "mul_poke") == 0)
            gpsp_serial_setting = SERIAL_MODE_SERIAL_POKE;
        else if (strcasecmp(value, "mul_aw1") == 0)
            gpsp_serial_setting = SERIAL_MODE_SERIAL_AW1;
        else if (strcasecmp(value, "mul_aw2") == 0)
            gpsp_serial_setting = SERIAL_MODE_SERIAL_AW2;
        return;
    }

    if (strcmp(key, "rtc_mode") == 0) {
        if (strcasecmp(value, "auto") == 0)
            gpsp_rtc_mode = FEAT_AUTODETECT;
        else if (strcasecmp(value, "enabled") == 0)
            gpsp_rtc_mode = FEAT_ENABLE;
        else if (strcasecmp(value, "disabled") == 0)
            gpsp_rtc_mode = FEAT_DISABLE;
        return;
    }

    if (strcmp(key, "web_server_enable") == 0) {
        int enabled;
        if (parse_bool_value(value, &enabled)) {
            gpsp_web_server_enabled = enabled;
        }
        return;
    }

    if (strcmp(key, "frameskip_interval") == 0) {
        parsed_long = strtol(value, &endptr, 10);
        if (endptr != value && parsed_long >= 0 && parsed_long <= 9) {
            gpsp_frameskip_interval = (uint32_t)parsed_long;
        }
        return;
    }

    if (strcmp(key, "frameskip_type") == 0) {
        parsed_long = strtol(value, &endptr, 10);
        if (endptr != value && parsed_long >= 0 && parsed_long <= 3) {
            gpsp_frameskip_type = (uint32_t)parsed_long;
        }
        return;
    }

    if (strcmp(key, "frameskip_threshold") == 0) {
        parsed_long = strtol(value, &endptr, 10);
        if (endptr != value && parsed_long >= 0 && parsed_long <= 100) {
            gpsp_frameskip_threshold = (uint32_t)parsed_long;
        }
        return;
    }

    ESP_LOGW(TAG, "Ignoring unknown or invalid config entry: %s=%s", key, value);
}

static const char *serial_setting_to_str(int setting)
{
    switch (setting) {
    case SERIAL_MODE_DISABLED:    return "disabled";
    case SERIAL_MODE_RFU:         return "rfu";
    case SERIAL_MODE_SERIAL_POKE: return "mul_poke";
    case SERIAL_MODE_SERIAL_AW1:  return "mul_aw1";
    case SERIAL_MODE_SERIAL_AW2:  return "mul_aw2";
    default:                      return "auto";
    }
}

static const char *rtc_mode_to_str(int mode)
{
    switch (mode) {
    case FEAT_DISABLE: return "disabled";
    case FEAT_ENABLE:  return "enabled";
    default:           return "auto";
    }
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
            "serial_mode=%s\n"
            "rtc_mode=%s\n"
            "web_server_enable=%d\n"
            "netplay_ra_enable=%d\n"
            "netplay_ra_mode=%d\n"
            "netplay_ra_host=%s\n"
            "netplay_ra_port=%u\n"
            "netplay_ra_nick=%s\n"
            "netplay_ra_tunnel_id=%s\n"
            "netplay_lobby_host=%s\n"
            "netplay_lobby_port=%u\n"
            "netplay_lobby_relay=%s\n"
            "frameskip_interval=%u\n"
            "frameskip_type=%u\n"
            "frameskip_threshold=%u\n",
            dynarec_enable ? 1 : 0,
            sprite_limit ? 1 : 0,
            selected_boot_mode == boot_bios ? 1 : 0,
            serial_setting_to_str(gpsp_serial_setting),
            rtc_mode_to_str(gpsp_rtc_mode),
            gpsp_web_server_enabled ? 1 : 0,
            gpsp_netplay_ra_enabled ? 1 : 0,
            gpsp_netplay_ra_mode,
            gpsp_netplay_ra_host,
            gpsp_netplay_ra_port,
            gpsp_netplay_ra_nick,
            gpsp_netplay_ra_tunnel_id,
            gpsp_netplay_lobby_host,
            gpsp_netplay_lobby_port,
            gpsp_netplay_lobby_relay,
            (unsigned)gpsp_frameskip_interval,
            (unsigned)gpsp_frameskip_type,
            (unsigned)gpsp_frameskip_threshold);
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
             "Runtime netplay: ra_enable=%d host=%s port=%u nick=%s",
             gpsp_netplay_ra_enabled ? 1 : 0,
             gpsp_netplay_ra_host,
             gpsp_netplay_ra_port,
             gpsp_netplay_ra_nick);
    ESP_LOGI(TAG,
             "Runtime lobby: host=%s port=%u relay=%s",
             gpsp_netplay_lobby_host,
             gpsp_netplay_lobby_port,
             gpsp_netplay_lobby_relay);
    return ESP_OK;
}