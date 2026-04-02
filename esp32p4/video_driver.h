/*
 * gpsp ESP32-P4 Platform — Video driver header
 * MIPI-DSI LCD (ST7701) + PPA hardware scaling + PWM backlight
 * Target board: JC4880P443C_I_W (480x800 ST7701 panel)
 */

#ifndef ESP32P4_VIDEO_DRIVER_H
#define ESP32P4_VIDEO_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* JC4880 display hardware pins */
#define VIDEO_LCD_BACKLIGHT_GPIO  23
#define VIDEO_LCD_RST_GPIO        5
#define VIDEO_MIPI_DSI_PHY_LDO_CHAN   3
#define VIDEO_MIPI_DSI_PHY_LDO_MV    2500

/* Display configuration */
typedef struct {
    uint16_t lcd_h_res;        /* LCD horizontal resolution (default 480) */
    uint16_t lcd_v_res;        /* LCD vertical resolution (default 800) */
    bool     use_ppa_scaling;  /* Enable PPA hardware scaling */
    int      num_fbs;          /* Number of DPI framebuffers (1 or 2) */
} video_driver_config_t;

/**
 * Initialize display subsystem:
 * - LDO power for MIPI DSI PHY
 * - MIPI-DSI bus + ST7701 panel
 * - PWM backlight
 * - PPA scaling client (optional)
 */
esp_err_t video_driver_init(const video_driver_config_t *config);

/**
 * Submit a completed GBA frame (240x160 RGB565) for display.
 * Uses PPA to scale and center the frame on the LCD.
 */
esp_err_t video_driver_submit_frame(const uint16_t *gba_framebuffer);

/**
 * Set LCD backlight brightness (0-100%).
 */
esp_err_t video_driver_set_brightness(int percent);

/**
 * Deinitialize display subsystem, free resources.
 */
void video_driver_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP32P4_VIDEO_DRIVER_H */
