/*
 * gpsp ESP32-P4 Platform — Input driver header
 * GPIO button polling (active-low with internal pull-ups)
 * Target board: JC4880P443C_I_W
 *
 * The JC4880 board has 4 buttons (S1-S4) and an expansion IO header (JP1).
 * For GBA gaming, external buttons must be wired to the expansion connector.
 * Default GPIOs use available expansion pins. Set -1 to disable unused buttons.
 */

#ifndef ESP32P4_INPUT_DRIVER_H
#define ESP32P4_INPUT_DRIVER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GBA button bitmask */
#define GBA_KEY_A       (1 << 0)
#define GBA_KEY_B       (1 << 1)
#define GBA_KEY_SELECT  (1 << 2)
#define GBA_KEY_START   (1 << 3)
#define GBA_KEY_RIGHT   (1 << 4)
#define GBA_KEY_LEFT    (1 << 5)
#define GBA_KEY_UP      (1 << 6)
#define GBA_KEY_DOWN    (1 << 7)
#define GBA_KEY_R       (1 << 8)
#define GBA_KEY_L       (1 << 9)

/* GPIO mapping for GBA buttons */
typedef struct {
    int gpio_a;
    int gpio_b;
    int gpio_select;
    int gpio_start;
    int gpio_right;
    int gpio_left;
    int gpio_up;
    int gpio_down;
    int gpio_r;
    int gpio_l;
} input_driver_config_t;

/**
 * Initialize GPIO inputs with pull-ups.
 */
esp_err_t input_driver_init(const input_driver_config_t *config);

/**
 * Read current button state. Returns bitmask of GBA_KEY_* values.
 * Bit = 1 means button is pressed.
 */
uint16_t input_driver_read(void);

/**
 * Deinitialize input driver.
 */
void input_driver_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP32P4_INPUT_DRIVER_H */
