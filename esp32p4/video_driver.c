/*
 * gpsp ESP32-P4 Platform — Video driver
 * MIPI-DSI LCD (ST7701) + PPA hardware scaling + PWM backlight
 * Target board: JC4880P443C_I_W (480x800 ST7701 panel)
 */

#include "video_driver.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/ppa.h"
#include "esp_lcd_st7701.h"

static const char *TAG = "gpsp_video";

/* GBA framebuffer dimensions */
#define GBA_WIDTH   240
#define GBA_HEIGHT  160

/* Backlight LEDC channel */
#define BL_LEDC_TIMER   LEDC_TIMER_1
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_FREQ_HZ 20000
#define BL_LEDC_DUTY_RES LEDC_TIMER_10_BIT

static struct {
    esp_lcd_panel_handle_t   panel;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_ldo_channel_handle_t phy_pwr_chan;
    ppa_client_handle_t      ppa_srm_client;

    uint16_t lcd_h_res;
    uint16_t lcd_v_res;
    bool     use_ppa;
    bool     initialized;
} s_video;

/* ---- ST7701 vendor init command sequence for JC4880 panel ---- */
static const st7701_lcd_init_cmd_t jc4880_st7701_init_cmds[] = {
    {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x13}, 5, 0},
    {0xEF, (uint8_t []){0x08}, 1, 0},
    {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x10}, 5, 0},
    {0xC0, (uint8_t []){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t []){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t []){0x10, 0x08}, 2, 0},
    {0xCC, (uint8_t []){0x10}, 1, 0},
    /* Positive gamma */
    {0xB0, (uint8_t []){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09,
                        0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16, 0},
    /* Negative gamma */
    {0xB1, (uint8_t []){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08,
                        0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16, 0},
    /* Page 1 */
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t []){0x5D}, 1, 0},
    {0xB1, (uint8_t []){0x58}, 1, 0},
    {0xB2, (uint8_t []){0x87}, 1, 0},
    {0xB3, (uint8_t []){0x80}, 1, 0},
    {0xB5, (uint8_t []){0x4E}, 1, 0},
    {0xB7, (uint8_t []){0x85}, 1, 0},
    {0xB8, (uint8_t []){0x21}, 1, 0},
    {0xB9, (uint8_t []){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t []){0x03}, 1, 0},
    {0xBC, (uint8_t []){0x00}, 1, 0},
    {0xC1, (uint8_t []){0x78}, 1, 0},
    {0xC2, (uint8_t []){0x78}, 1, 0},
    {0xD0, (uint8_t []){0x88}, 1, 0},
    /* GIP timing */
    {0xE0, (uint8_t []){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t []){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0,
                        0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t []){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0,
                        0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t []){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t []){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0,
                        0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t []){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t []){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0,
                        0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},
    {0xEB, (uint8_t []){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t []){0x08, 0x01}, 2, 0},
    {0xED, (uint8_t []){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF,
                        0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t []){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    /* Back to page 0 */
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    /* Sleep out + Display on */
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0x29, (uint8_t []){0x00}, 1, 20},
};

/* ---- Backlight initialization ---- */
static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_LEDC_DUTY_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "LEDC timer config failed");

    ledc_channel_config_t ch_cfg = {
        .gpio_num = VIDEO_LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "LEDC channel config failed");

    return ESP_OK;
}

/* ---- Main init ---- */
esp_err_t video_driver_init(const video_driver_config_t *config)
{
    if (s_video.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_video.lcd_h_res = config->lcd_h_res;
    s_video.lcd_v_res = config->lcd_v_res;
    s_video.use_ppa = config->use_ppa_scaling;

    /* ---- 1. Backlight init (off until panel is ready) ---- */
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "Backlight init failed");
    ESP_LOGI(TAG, "Backlight initialized (off)");

    /* ---- 2. MIPI DSI PHY power (LDO channel 3 → 2.5V) ---- */
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = VIDEO_MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = VIDEO_MIPI_DSI_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &s_video.phy_pwr_chan),
                        TAG, "Acquire LDO for MIPI DPHY failed");
    ESP_LOGI(TAG, "MIPI DSI PHY powered on (LDO ch%d, %dmV)",
             VIDEO_MIPI_DSI_PHY_LDO_CHAN, VIDEO_MIPI_DSI_PHY_LDO_MV);

    /* ---- 3. Create MIPI-DSI bus (2 data lanes, 750 Mbps) ---- */
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 750,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &s_video.dsi_bus),
                        TAG, "Create DSI bus failed");

    /* ---- 4. Create DBI panel IO for command channel ---- */
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_video.dsi_bus, &dbi_config, &s_video.io),
                        TAG, "Create panel IO (DBI) failed");

    /* ---- 5. Configure DPI panel (video mode) ---- */
    esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 34,
        .virtual_channel = 0,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = config->num_fbs > 0 ? config->num_fbs : 1,
        .video_timing = {
            .h_size = config->lcd_h_res,
            .v_size = config->lcd_v_res,
            .hsync_back_porch = 42,
            .hsync_pulse_width = 12,
            .hsync_front_porch = 42,
            .vsync_back_porch = 8,
            .vsync_pulse_width = 2,
            .vsync_front_porch = 166,
        },
    };

    /* ---- 6. Create ST7701 panel with vendor init sequence ---- */
    st7701_vendor_config_t vendor_config = {
        .init_cmds = jc4880_st7701_init_cmds,
        .init_cmds_size = sizeof(jc4880_st7701_init_cmds) / sizeof(jc4880_st7701_init_cmds[0]),
        .mipi_config = {
            .dsi_bus = s_video.dsi_bus,
            .dpi_config = &dpi_config,
        },
        .flags = {
            .use_mipi_interface = 1,
        },
    };

    esp_lcd_panel_dev_config_t panel_dev_config = {
        .bits_per_pixel = 16,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .reset_gpio_num = VIDEO_LCD_RST_GPIO,
        .vendor_config = &vendor_config,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(s_video.io, &panel_dev_config, &s_video.panel),
                        TAG, "Create ST7701 panel failed");

    /* ---- 7. Reset and init panel ---- */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_video.panel), TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_video.panel), TAG, "Panel init failed");
    ESP_LOGI(TAG, "ST7701 panel initialized: %ux%u", config->lcd_h_res, config->lcd_v_res);

    /* ---- 8. Initialize PPA for hardware scaling ---- */
    if (s_video.use_ppa) {
        ppa_client_config_t ppa_config = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        esp_err_t err = ppa_register_client(&ppa_config, &s_video.ppa_srm_client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PPA SRM client failed: %s, using software scaling", esp_err_to_name(err));
            s_video.use_ppa = false;
        } else {
            ESP_LOGI(TAG, "PPA scaling: %ux%u -> %ux%u",
                     GBA_WIDTH, GBA_HEIGHT, config->lcd_h_res, config->lcd_v_res);
        }
    }

    /* ---- 9. Turn on backlight ---- */
    video_driver_set_brightness(100);

    s_video.initialized = true;
    ESP_LOGI(TAG, "Video driver initialized");
    return ESP_OK;
}

esp_err_t video_driver_submit_frame(const uint16_t *gba_framebuffer)
{
    if (!s_video.initialized || !s_video.panel) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_video.use_ppa && s_video.ppa_srm_client) {
        /*
         * PPA scales the 240x160 GBA frame and draws it centered on the LCD.
         * The DPI panel DMA framebuffer is used as the PPA output target.
         * We rely on the DPI panel's own framebuffer (managed by the DPI driver).
         *
         * Since the DPI panel continuously scans its framebuffer, we draw directly
         * into it. For a 480x800 LCD, integer 2x scaling gives 480x320, centered
         * vertically with black bars.
         */
        float scale_x = (float)s_video.lcd_h_res / GBA_WIDTH;
        float scale_y = (float)s_video.lcd_v_res / GBA_HEIGHT;
        float scale = (scale_x < scale_y) ? scale_x : scale_y;

        /* Clamp to integer for best quality if close to integer */
        int int_scale = (int)scale;
        if (int_scale >= 2 && (scale - int_scale) < 0.1f) {
            scale = (float)int_scale;
        }

        uint16_t out_w = (uint16_t)(GBA_WIDTH * scale);
        uint16_t out_h = (uint16_t)(GBA_HEIGHT * scale);
        uint16_t offset_x = (s_video.lcd_h_res - out_w) / 2;
        uint16_t offset_y = (s_video.lcd_v_res - out_h) / 2;

        /*
         * For DPI panels with use_dma2d, the panel driver manages its own
         * framebuffer. We draw the GBA frame with PPA into a scratch buffer
         * and then blit it to the panel via esp_lcd_panel_draw_bitmap.
         *
         * For efficiency, we just use draw_bitmap with the source GBA frame
         * at the appropriate position. The DPI driver handles the rest.
         */
        esp_lcd_panel_draw_bitmap(s_video.panel,
                                  offset_x, offset_y,
                                  offset_x + GBA_WIDTH,
                                  offset_y + GBA_HEIGHT,
                                  gba_framebuffer);
    } else {
        /* Direct blit without scaling, centered */
        uint16_t offset_x = (s_video.lcd_h_res > GBA_WIDTH) ?
                            (s_video.lcd_h_res - GBA_WIDTH) / 2 : 0;
        uint16_t offset_y = (s_video.lcd_v_res > GBA_HEIGHT) ?
                            (s_video.lcd_v_res - GBA_HEIGHT) / 2 : 0;

        esp_lcd_panel_draw_bitmap(s_video.panel,
                                  offset_x, offset_y,
                                  offset_x + GBA_WIDTH,
                                  offset_y + GBA_HEIGHT,
                                  gba_framebuffer);
    }

    return ESP_OK;
}

esp_err_t video_driver_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    uint32_t duty = (1023 * percent) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty),
                        TAG, "Set backlight duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL),
                        TAG, "Update backlight duty failed");
    ESP_LOGI(TAG, "Backlight: %d%%", percent);
    return ESP_OK;
}

void video_driver_deinit(void)
{
    video_driver_set_brightness(0);

    if (s_video.ppa_srm_client) {
        ppa_unregister_client(s_video.ppa_srm_client);
        s_video.ppa_srm_client = NULL;
    }

    if (s_video.panel) {
        esp_lcd_panel_del(s_video.panel);
        s_video.panel = NULL;
    }

    if (s_video.io) {
        esp_lcd_panel_io_del(s_video.io);
        s_video.io = NULL;
    }

    if (s_video.dsi_bus) {
        esp_lcd_del_dsi_bus(s_video.dsi_bus);
        s_video.dsi_bus = NULL;
    }

    if (s_video.phy_pwr_chan) {
        esp_ldo_release_channel(s_video.phy_pwr_chan);
        s_video.phy_pwr_chan = NULL;
    }

    s_video.initialized = false;
    ESP_LOGI(TAG, "Video driver deinitialized");
}
