/*
 * gpsp ESP32-P4 Platform — Input driver
 * GPIO button polling (active-low with internal pull-ups)
 * Target board: JC4880P443C_I_W
 */

#include "input_driver.h"

#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "gpsp_input";

typedef struct {
    uint16_t key_mask;
    int      gpio_num;
} key_gpio_map_t;

#define MAX_KEYS 10

static struct {
    key_gpio_map_t map[MAX_KEYS];
    int            num_keys;
    bool           initialized;
} s_input;

esp_err_t input_driver_init(const input_driver_config_t *config)
{
    if (s_input.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const struct { uint16_t mask; int gpio; } cfg_map[] = {
        { GBA_KEY_A,      config->gpio_a      },
        { GBA_KEY_B,      config->gpio_b      },
        { GBA_KEY_SELECT, config->gpio_select  },
        { GBA_KEY_START,  config->gpio_start   },
        { GBA_KEY_RIGHT,  config->gpio_right   },
        { GBA_KEY_LEFT,   config->gpio_left    },
        { GBA_KEY_UP,     config->gpio_up      },
        { GBA_KEY_DOWN,   config->gpio_down    },
        { GBA_KEY_R,      config->gpio_r       },
        { GBA_KEY_L,      config->gpio_l       },
    };

    s_input.num_keys = 0;

    for (int i = 0; i < MAX_KEYS; i++) {
        if (cfg_map[i].gpio < 0) {
            continue;
        }

        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << cfg_map[i].gpio),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure GPIO %d: %s",
                     cfg_map[i].gpio, esp_err_to_name(err));
            continue;
        }

        s_input.map[s_input.num_keys].key_mask = cfg_map[i].mask;
        s_input.map[s_input.num_keys].gpio_num = cfg_map[i].gpio;
        s_input.num_keys++;
    }

    s_input.initialized = true;
    ESP_LOGI(TAG, "Input driver initialized: %d buttons configured", s_input.num_keys);
    return ESP_OK;
}

uint16_t input_driver_read(void)
{
    uint16_t keys = 0;

    for (int i = 0; i < s_input.num_keys; i++) {
        if (gpio_get_level(s_input.map[i].gpio_num) == 0) {
            keys |= s_input.map[i].key_mask;
        }
    }

    return keys;
}

void input_driver_deinit(void)
{
    for (int i = 0; i < s_input.num_keys; i++) {
        gpio_reset_pin(s_input.map[i].gpio_num);
    }
    s_input.num_keys = 0;
    s_input.initialized = false;
    ESP_LOGI(TAG, "Input driver deinitialized");
}
