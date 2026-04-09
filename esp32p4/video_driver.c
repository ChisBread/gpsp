/*
 * gpsp ESP32-P4 Platform — Video driver
 * MIPI-DSI LCD (ST7701) + PPA hardware scaling + PWM backlight
 * Target board: JC4880P443C_I_W (480x800 ST7701 panel)
 */

#include "video_driver.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
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

/*
 * Output pipeline: GBA 240x160 → rotate 90° CCW → 160x240 → ×3 → 480x720
 * Centered on 480x800 LCD with 40px black bars top/bottom.
 * Uses PPA hardware when available, software fallback otherwise.
 */
#define SCALE_FACTOR    3
#define SCALED_W        (GBA_HEIGHT * SCALE_FACTOR)  /* 160*3 = 480 */
#define SCALED_H        (GBA_WIDTH  * SCALE_FACTOR)  /* 240*3 = 720 */

static struct {
    esp_lcd_panel_handle_t   panel;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_ldo_channel_handle_t phy_pwr_chan;
    ppa_client_handle_t      ppa_srm_client;

    uint16_t *out_buf;           /* Current draw target (may be a DPI panel FB) */
    uint16_t *dpi_fbs[3];        /* DPI panel framebuffers (up to triple-buffered) */
    int       dpi_draw_idx;      /* Index of the FB we are drawing into */
    int       dpi_fb_count;      /* Number of DPI FBs (1, 2, or 3) */
    size_t    out_buf_size;
    uint16_t  offset_y;          /* Vertical centering offset: (800-720)/2 = 40 */

    uint16_t lcd_h_res;
    uint16_t lcd_v_res;
    bool     use_ppa;
    bool     direct_dpi_fb;     /* true → out_buf IS a DPI panel FB */
    bool     ppa_async;         /* true → PPA runs non-blocking */
    bool     ppa_pending;       /* a non-blocking PPA is in flight */
    SemaphoreHandle_t ppa_done; /* signalled from PPA ISR callback */
    SemaphoreHandle_t vsync;    /* signalled from DPI refresh-done ISR */
    bool     initialized;
} s_video;

/*
 * Software rotate 90° CCW + 3x nearest-neighbor scale.
 * Input:  240(W)x160(H) row-major RGB565
 * Output: 480x720 placed into lcd_w x lcd_h buffer at (0, offset_y)
 *
 * CCW 90°: src(x, y) → dst(y, W-1-x)   (in 160×240 space)
 * Then ×3: each dst pixel → 3×3 block in final output.
 */
static void sw_rotate_scale(const uint16_t *src, uint16_t *dst,
                            uint16_t lcd_w, uint16_t offset_y)
{
    for (int y = 0; y < GBA_HEIGHT; y++) {
        const uint16_t *src_row = src + y * GBA_WIDTH;
        for (int x = 0; x < GBA_WIDTH; x++) {
            uint16_t pixel = src_row[x];
            /* Rotated position (before scaling): (y, GBA_WIDTH-1-x) */
            int out_x = y * SCALE_FACTOR;
            int out_y = (GBA_WIDTH - 1 - x) * SCALE_FACTOR + offset_y;
            uint16_t *p = dst + out_y * lcd_w + out_x;
            p[0] = p[1] = p[2] = pixel;
            p += lcd_w;
            p[0] = p[1] = p[2] = pixel;
            p += lcd_w;
            p[0] = p[1] = p[2] = pixel;
        }
    }
}

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

/* PPA transaction-done ISR callback (non-blocking mode). */
static bool ppa_trans_done_cb(ppa_client_handle_t client,
                              ppa_event_data_t *event_data,
                              void *user_data)
{
    (void)client; (void)event_data; (void)user_data;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_video.ppa_done, &woken);
    return (woken == pdTRUE);
}

/* DPI refresh-done ISR callback — fires once per panel VSYNC. */
static bool IRAM_ATTR dpi_refresh_done_cb(esp_lcd_panel_handle_t panel,
                                          esp_lcd_dpi_panel_event_data_t *edata,
                                          void *user_ctx)
{
    (void)panel; (void)edata; (void)user_ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_video.vsync, &woken);
    return (woken == pdTRUE);
}

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
    /*
     * DPI clock: PLL_F240M / 7 = 34.2857 MHz (integer divider).
     * Blanking is tuned so that h_total × v_total = 577 × 995 = 574115,
     * giving a refresh rate of 34285714 / 574115 ≈ 59.7274 Hz — matching
     * the GBA's 59.7275 Hz to within 0.0002 %.
     */
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
            .hsync_front_porch = 43,
            .vsync_back_porch = 8,
            .vsync_pulse_width = 2,
            .vsync_front_porch = 185,
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

    /* ---- 7b. Register DPI VSYNC callback for tear-free swap ---- */
    s_video.vsync = xSemaphoreCreateBinary();
    if (s_video.vsync) {
        esp_lcd_dpi_panel_event_callbacks_t dpi_cbs = {
            .on_refresh_done = dpi_refresh_done_cb,
        };
        esp_err_t vcb_err = esp_lcd_dpi_panel_register_event_callbacks(s_video.panel,
                                                                       &dpi_cbs, NULL);
        if (vcb_err != ESP_OK) {
            ESP_LOGW(TAG, "DPI vsync callback registration failed: %s",
                     esp_err_to_name(vcb_err));
            vSemaphoreDelete(s_video.vsync);
            s_video.vsync = NULL;
        } else {
            ESP_LOGI(TAG, "DPI VSYNC callback registered — tear-free swap enabled");
        }
    }

    /* ---- 8. Scaled output buffer ---- */
    s_video.out_buf_size = config->lcd_h_res * config->lcd_v_res * sizeof(uint16_t);
    s_video.offset_y = (config->lcd_v_res > SCALED_H)
        ? (config->lcd_v_res - SCALED_H) / 2 : 0;

    /*
     * Try to get the DPI panel's internal framebuffer(s) so PPA can output
     * directly into them, eliminating the ~768 KB CPU memcpy in draw_bitmap.
     *
     * With 2 FBs: tear-free double buffering (draw back, display front, swap).
     * With 3 FBs: triple buffering — decouples render timing from display
     * timing, eliminating per-frame jitter visible in side-scrolling games.
     * The DPI controller always scans from one buffer; PPA writes into the
     * next free buffer; the third is the "just completed" frame queued for
     * display.  This absorbs render-time variance without frame drops.
     */
    s_video.dpi_fb_count = 0;
    s_video.dpi_draw_idx = 0;
    s_video.dpi_fbs[0] = NULL;
    s_video.dpi_fbs[1] = NULL;
    s_video.dpi_fbs[2] = NULL;
    s_video.direct_dpi_fb = false;

    int requested_fbs = config->num_fbs > 0 ? config->num_fbs : 1;
    if (requested_fbs >= 3) {
        void *fb0 = NULL, *fb1 = NULL, *fb2 = NULL;
        if (esp_lcd_dpi_panel_get_frame_buffer(s_video.panel, 3, &fb0, &fb1, &fb2) == ESP_OK
            && fb0 && fb1 && fb2) {
            s_video.dpi_fbs[0] = (uint16_t *)fb0;
            s_video.dpi_fbs[1] = (uint16_t *)fb1;
            s_video.dpi_fbs[2] = (uint16_t *)fb2;
            s_video.dpi_fb_count = 3;
            s_video.dpi_draw_idx = 1;  /* fb0 displayed, start drawing into fb1 */
            s_video.out_buf = s_video.dpi_fbs[s_video.dpi_draw_idx];
            s_video.direct_dpi_fb = true;
            for (int i = 0; i < 3; i++) {
                memset(s_video.dpi_fbs[i], 0, s_video.out_buf_size);
                esp_cache_msync(s_video.dpi_fbs[i], s_video.out_buf_size,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            }
            ESP_LOGI(TAG, "Direct DPI FB x3 — triple-buffered PPA -> LCD");
        } else {
            /* Fall back to requesting 2 */
            requested_fbs = 2;
        }
    }

    if (!s_video.direct_dpi_fb && requested_fbs >= 2) {
        void *fb0 = NULL, *fb1 = NULL;
        if (esp_lcd_dpi_panel_get_frame_buffer(s_video.panel, 2, &fb0, &fb1) == ESP_OK
            && fb0 && fb1) {
            s_video.dpi_fbs[0] = (uint16_t *)fb0;
            s_video.dpi_fbs[1] = (uint16_t *)fb1;
            s_video.dpi_fb_count = 2;
            s_video.dpi_draw_idx = 1;  /* draw into fb1 first, fb0 is displayed */
            s_video.out_buf = s_video.dpi_fbs[s_video.dpi_draw_idx];
            s_video.direct_dpi_fb = true;
            for (int i = 0; i < 2; i++) {
                memset(s_video.dpi_fbs[i], 0, s_video.out_buf_size);
                esp_cache_msync(s_video.dpi_fbs[i], s_video.out_buf_size,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            }
            ESP_LOGI(TAG, "Direct DPI FB x2 — tear-free PPA -> LCD");
        }
    }

    if (!s_video.direct_dpi_fb) {
        void *fb0 = NULL;
        if (esp_lcd_dpi_panel_get_frame_buffer(s_video.panel, 1, &fb0) == ESP_OK
            && fb0) {
            s_video.dpi_fbs[0] = (uint16_t *)fb0;
            s_video.dpi_fb_count = 1;
            s_video.out_buf = (uint16_t *)fb0;
            s_video.direct_dpi_fb = true;
            memset(s_video.out_buf, 0, s_video.out_buf_size);
            esp_cache_msync(s_video.out_buf, s_video.out_buf_size,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            ESP_LOGI(TAG, "Direct DPI FB x1 — zero-copy PPA -> LCD");
        } else {
            s_video.out_buf = (uint16_t *)heap_caps_aligned_alloc(
                64, s_video.out_buf_size, MALLOC_CAP_SPIRAM);
            if (!s_video.out_buf) {
                ESP_LOGE(TAG, "Output buffer alloc failed (%u bytes)",
                         (unsigned)s_video.out_buf_size);
                return ESP_ERR_NO_MEM;
            }
            memset(s_video.out_buf, 0, s_video.out_buf_size);
        }
    }

    /* ---- 9. Initialize PPA for hardware rotation + scaling (optional) ---- */
    s_video.ppa_async = false;
    s_video.ppa_pending = false;
    s_video.ppa_done = NULL;

    if (s_video.use_ppa) {
        ppa_client_config_t ppa_config = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        esp_err_t err = ppa_register_client(&ppa_config, &s_video.ppa_srm_client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PPA SRM client failed: %s, using software fallback", esp_err_to_name(err));
            s_video.use_ppa = false;
        } else {
            /* Enable non-blocking PPA when multi-buffered DPI is available
             * so PPA DMA runs in parallel with other render work. */
            if (s_video.direct_dpi_fb && s_video.dpi_fb_count >= 2) {
                s_video.ppa_done = xSemaphoreCreateBinary();
                if (s_video.ppa_done) {
                    ppa_event_callbacks_t cbs = {
                        .on_trans_done = ppa_trans_done_cb,
                    };
                    err = ppa_client_register_event_callbacks(s_video.ppa_srm_client, &cbs);
                    if (err == ESP_OK) {
                        s_video.ppa_async = true;
                        ESP_LOGI(TAG, "PPA async mode enabled (double-buffered DPI)");
                    } else {
                        vSemaphoreDelete(s_video.ppa_done);
                        s_video.ppa_done = NULL;
                        ESP_LOGW(TAG, "PPA callback reg failed, using blocking: %s",
                                 esp_err_to_name(err));
                    }
                }
            }
            ESP_LOGI(TAG, "PPA: %ux%u → rotate 90° → x%d → %ux%u, centered at y=%u",
                     GBA_WIDTH, GBA_HEIGHT, SCALE_FACTOR,
                     SCALED_W, SCALED_H, s_video.offset_y);
        }
    }
    if (!s_video.use_ppa) {
        ESP_LOGI(TAG, "SW: %ux%u → rotate 90° → x%d → %ux%u, centered at y=%u",
                 GBA_WIDTH, GBA_HEIGHT, SCALE_FACTOR,
                 SCALED_W, SCALED_H, s_video.offset_y);
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
        /* PPA hardware path: rotate 90° CCW + scale ×3 in one pass */
        ppa_srm_oper_config_t srm_config = {
            .in = {
                .buffer = (const void *)gba_framebuffer,
                .pic_w = GBA_WIDTH,
                .pic_h = GBA_HEIGHT,
                .block_w = GBA_WIDTH,
                .block_h = GBA_HEIGHT,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = s_video.out_buf,
                .buffer_size = s_video.out_buf_size,
                .pic_w = s_video.lcd_h_res,
                .pic_h = s_video.lcd_v_res,
                .block_offset_x = 0,
                .block_offset_y = s_video.offset_y,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
            .scale_x = (float)SCALE_FACTOR,
            .scale_y = (float)SCALE_FACTOR,
            .rgb_swap = false,
            .byte_swap = false,
            .mode = s_video.ppa_async ? PPA_TRANS_MODE_NON_BLOCKING
                                      : PPA_TRANS_MODE_BLOCKING,
        };

        esp_err_t err = ppa_do_scale_rotate_mirror(s_video.ppa_srm_client, &srm_config);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PPA SRM failed: %s", esp_err_to_name(err));
            return err;
        }

        if (s_video.ppa_async) {
            s_video.ppa_pending = true;
            return ESP_OK;  /* DPI swap deferred to await_frame */
        }
    } else {
        /* Software path: rotate 90° CCW + 3x nearest-neighbor scale */
        sw_rotate_scale(gba_framebuffer, s_video.out_buf,
                        s_video.lcd_h_res, s_video.offset_y);
    }

    /* Synchronous completion path (SW fallback or blocking PPA) */
    if (s_video.direct_dpi_fb) {
        if (!(s_video.use_ppa && s_video.ppa_srm_client)) {
            esp_cache_msync(s_video.out_buf, s_video.out_buf_size,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }

        if (s_video.dpi_fb_count >= 2) {
            esp_lcd_panel_draw_bitmap(s_video.panel,
                                      0, 0,
                                      s_video.lcd_h_res, s_video.lcd_v_res,
                                      s_video.out_buf);
            if (s_video.vsync) {
                while (xSemaphoreTake(s_video.vsync, 0) == pdTRUE) {}
                xSemaphoreTake(s_video.vsync, portMAX_DELAY);
            }
            s_video.dpi_draw_idx = (s_video.dpi_draw_idx + 1) % s_video.dpi_fb_count;
            s_video.out_buf = s_video.dpi_fbs[s_video.dpi_draw_idx];
        }
    } else {
        esp_lcd_panel_draw_bitmap(s_video.panel,
                                  0, 0,
                                  s_video.lcd_h_res, s_video.lcd_v_res,
                                  s_video.out_buf);
    }

    return ESP_OK;
}

esp_err_t video_driver_await_frame(void)
{
    if (!s_video.ppa_pending)
        return ESP_OK;

    /* Wait for the async PPA DMA to finish. */
    xSemaphoreTake(s_video.ppa_done, portMAX_DELAY);
    s_video.ppa_pending = false;

    /* Schedule the buffer switch: draw_bitmap sets cur_fb_index so
     * the DPI DMA ISR will pick it up at the next frame boundary. */
    esp_lcd_panel_draw_bitmap(s_video.panel,
                              0, 0,
                              s_video.lcd_h_res, s_video.lcd_v_res,
                              s_video.out_buf);

    if (s_video.dpi_fb_count >= 3) {
        /* Triple buffering: pace to VSYNC but do NOT drain stale
         * semaphore counts first.  With 3 rotating buffers the draw
         * target is always 2 positions behind — guaranteed free after
         * two VSYNC switches.  By skipping the drain we let a frame
         * that finished *after* the VSYNC deadline consume the
         * already-given semaphore instantly instead of waiting for the
         * NEXT VSYNC (which would impose the same 2-VSYNC penalty as
         * double buffering).
         *
         * Fast frame (done before VSYNC): blocks here until VSYNC
         *   → paced to ~59.7 Hz, no tearing.
         * Late frame (done after VSYNC):  semaphore already given,
         *   take returns immediately → next frame starts without
         *   extra latency, eliminating the scroll-jitter visible in
         *   side-scrolling games like Kirby. */
        if (s_video.vsync) {
            xSemaphoreTake(s_video.vsync, portMAX_DELAY);
        }
        s_video.dpi_draw_idx = (s_video.dpi_draw_idx + 1) % s_video.dpi_fb_count;
        s_video.out_buf = s_video.dpi_fbs[s_video.dpi_draw_idx];
    } else {
        /* Double buffering: must wait for VSYNC to confirm the DPI
         * controller switched away from our target buffer. */
        if (s_video.vsync) {
            while (xSemaphoreTake(s_video.vsync, 0) == pdTRUE) {}
            xSemaphoreTake(s_video.vsync, portMAX_DELAY);
        }
        s_video.dpi_draw_idx = (s_video.dpi_draw_idx + 1) % s_video.dpi_fb_count;
        s_video.out_buf = s_video.dpi_fbs[s_video.dpi_draw_idx];
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

    if (s_video.out_buf && !s_video.direct_dpi_fb) {
        heap_caps_free(s_video.out_buf);
        s_video.out_buf = NULL;
    }

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
