/*
 * gpsp ESP32-P4 Platform — Audio driver
 * ES8311 Codec (I2C control + I2S data) with speaker PA
 * Target board: JC4880P443C_I_W
 *
 * The ES8311 is controlled via I2C for configuration and uses I2S for
 * audio data. The PA (power amplifier) is enabled via GPIO11.
 */

#include "audio_driver.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

static const char *TAG = "gpsp_audio";

/* One GBA frame produces roughly 550-900 stereo sample pairs depending on
 * the configured audio rate. Keep each DMA buffer under the ESP-IDF I2S
 * per-descriptor byte limit to avoid the driver silently clamping it and
 * emitting a warning at startup. */
#define AUDIO_DMA_DESC_NUM   8
#define AUDIO_DMA_FRAME_NUM  1008

/* ---- ES8311 register definitions (subset for playback) ---- */
#define ES8311_REG_RESET        0x00
#define ES8311_REG_CLK_MGR1     0x01
#define ES8311_REG_CLK_MGR2     0x02
#define ES8311_REG_CLK_MGR3     0x03
#define ES8311_REG_CLK_MGR4     0x04
#define ES8311_REG_CLK_MGR5     0x05
#define ES8311_REG_CLK_MGR6     0x06
#define ES8311_REG_CLK_MGR7     0x07
#define ES8311_REG_CLK_MGR8     0x08
#define ES8311_REG_SDP_IN       0x09
#define ES8311_REG_SDP_OUT      0x0A
#define ES8311_REG_SYSTEM       0x0D
#define ES8311_REG_SYSTEM2      0x0E
#define ES8311_REG_SYSTEM3      0x0F
#define ES8311_REG_ADC1         0x10
#define ES8311_REG_ADC2         0x11
#define ES8311_REG_DAC1         0x12
#define ES8311_REG_DAC2         0x13
#define ES8311_REG_GPIO_CFG     0x14
#define ES8311_REG_GP           0x15
#define ES8311_REG_CHIP_ID1     0xFD
#define ES8311_REG_CHIP_ID2     0xFE
#define ES8311_REG_CHIP_VER     0xFF

static struct {
    i2s_chan_handle_t        tx_chan;
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t i2c_dev;
    uint32_t                sample_rate;
    bool                    initialized;
} s_audio;

/* ---- I2C helpers ---- */
static esp_err_t es8311_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t data[2] = {reg, val};
    return i2c_master_transmit(s_audio.i2c_dev, data, 2, 100);
}

static esp_err_t es8311_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_audio.i2c_dev, &reg, 1, val, 1, 100);
}

/* ---- ES8311 initialization for DAC playback ---- */
static esp_err_t es8311_codec_init(uint32_t sample_rate)
{
    uint8_t chip_id1, chip_id2;
    es8311_read_reg(ES8311_REG_CHIP_ID1, &chip_id1);
    es8311_read_reg(ES8311_REG_CHIP_ID2, &chip_id2);
    ESP_LOGI(TAG, "ES8311 Chip ID: 0x%02X 0x%02X", chip_id1, chip_id2);

    /* Reset codec */
    es8311_write_reg(ES8311_REG_RESET, 0x1F);
    es8311_write_reg(ES8311_REG_RESET, 0x00);

    /*
     * Clock configuration:
     * - MCLK from ESP32-P4 I2S MCLK output
     * - Slave mode (BCLK/WS from ESP32-P4)
     * - MCLK divider auto
     */
    es8311_write_reg(ES8311_REG_CLK_MGR1, 0x30);  /* MCLK source = pin, clock on */
    es8311_write_reg(ES8311_REG_CLK_MGR2, 0x00);   /* Auto MCLK divider */
    es8311_write_reg(ES8311_REG_CLK_MGR3, 0x10);   /* LRCK divider */
    es8311_write_reg(ES8311_REG_CLK_MGR4, 0x10);   /* LRCK divider */
    es8311_write_reg(ES8311_REG_CLK_MGR5, 0x00);   /* BCLK divider */
    es8311_write_reg(ES8311_REG_CLK_MGR6, 0x01);   /* BCLK = MCLK / N */
    es8311_write_reg(ES8311_REG_CLK_MGR7, 0x00);   /* Clocks */
    es8311_write_reg(ES8311_REG_CLK_MGR8, 0xFF);   /* Multi-clk */

    /* I2S format: 16-bit, I2S/Philips standard */
    es8311_write_reg(ES8311_REG_SDP_IN, 0x0C);    /* 16-bit word length, I2S format */
    es8311_write_reg(ES8311_REG_SDP_OUT, 0x0C);   /* 16-bit word length, I2S format */

    /* System control */
    es8311_write_reg(ES8311_REG_SYSTEM, 0x10);    /* Power up analog */
    es8311_write_reg(ES8311_REG_SYSTEM2, 0x00);   /* Power up DAC */
    es8311_write_reg(ES8311_REG_SYSTEM3, 0x10);   /* Enable vmid */

    /* DAC control */
    es8311_write_reg(ES8311_REG_DAC1, 0x02);      /* DAC unmute, enable DLL */
    es8311_write_reg(ES8311_REG_DAC2, 0xA0);      /* DAC volume: -16dB default */

    /* GPIO config */
    es8311_write_reg(ES8311_REG_GPIO_CFG, 0x00);
    es8311_write_reg(ES8311_REG_GP, 0x00);

    ESP_LOGI(TAG, "ES8311 codec initialized for %lu Hz playback", (unsigned long)sample_rate);
    return ESP_OK;
}

/* ---- PA control ---- */
static void pa_enable(bool enable)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << AUDIO_PA_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(AUDIO_PA_GPIO, enable ? 1 : 0);
    ESP_LOGI(TAG, "PA %s", enable ? "enabled" : "disabled");
}

/* ---- Public API ---- */
esp_err_t audio_driver_init(const audio_driver_config_t *config)
{
    if (s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_audio.sample_rate = config->sample_rate;

    /* ---- 1. Initialize I2C bus for ES8311 control ---- */
    i2c_master_bus_config_t i2c_bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_I2C_SDA_GPIO,
        .scl_io_num = AUDIO_I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &s_audio.i2c_bus));

    i2c_device_config_t i2c_dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_audio.i2c_bus, &i2c_dev_cfg, &s_audio.i2c_dev));

    /* ---- 2. Initialize I2S ---- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = AUDIO_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = AUDIO_DMA_FRAME_NUM;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_audio.tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_GPIO,
            .bclk = AUDIO_I2S_BCLK_GPIO,
            .ws   = AUDIO_I2S_WS_GPIO,
            .dout = AUDIO_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_audio.tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_audio.tx_chan));

    /* ---- 3. Initialize ES8311 codec via I2C ---- */
    ESP_ERROR_CHECK(es8311_codec_init(config->sample_rate));

    /* ---- 4. Enable PA ---- */
    pa_enable(true);

    s_audio.initialized = true;
    ESP_LOGI(TAG, "Audio driver initialized: %lu Hz stereo",
             (unsigned long)config->sample_rate);
    return ESP_OK;
}

esp_err_t audio_driver_write(const int16_t *samples, size_t count)
{
    if (!s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes_to_write = count * 2 * sizeof(int16_t);  /* stereo pairs */
    size_t bytes_written = 0;

    return i2s_channel_write(s_audio.tx_chan, samples, bytes_to_write,
                             &bytes_written, portMAX_DELAY);
}

esp_err_t audio_driver_set_volume(int volume_percent)
{
    if (!s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;

    /* ES8311 DAC volume register (0x32): 0x00 = 0dB, 0xFF = -95.5dB
     * Map 0-100% to useful range 0xBF(mute-ish) to 0x00(max) */
    uint8_t reg_val = (uint8_t)((100 - volume_percent) * 191 / 100);
    return es8311_write_reg(ES8311_REG_DAC2, reg_val);
}

void audio_driver_deinit(void)
{
    pa_enable(false);

    if (s_audio.tx_chan) {
        i2s_channel_disable(s_audio.tx_chan);
        i2s_del_channel(s_audio.tx_chan);
        s_audio.tx_chan = NULL;
    }

    if (s_audio.i2c_dev) {
        i2c_master_bus_rm_device(s_audio.i2c_dev);
        s_audio.i2c_dev = NULL;
    }

    if (s_audio.i2c_bus) {
        i2c_del_master_bus(s_audio.i2c_bus);
        s_audio.i2c_bus = NULL;
    }

    s_audio.initialized = false;
    ESP_LOGI(TAG, "Audio driver deinitialized");
}
