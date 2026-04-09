/*
 * gpsp ESP32-P4 Platform — Audio driver
 * ES8311 codec over I2C + I2S for JC4880P443C_I_W
 */

#include "audio_driver.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include <string.h>

static const char *TAG = "gpsp_audio";

#define AUDIO_DMA_DESC_NUM  8
#define AUDIO_DMA_FRAME_NUM 1008
#define AUDIO_MCLK_MULTIPLE 256
#define AUDIO_MAX_INPUT_FRAMES  ((AUDIO_PCM_SOURCE_RATE / 50) + 4)
#define AUDIO_MAX_OUTPUT_FRAMES ((((uint64_t)AUDIO_MAX_INPUT_FRAMES * 96000u) / 32000u) + 4)

static struct {
    i2s_chan_handle_t tx_chan;
    i2s_chan_handle_t rx_chan;
    i2c_master_bus_handle_t i2c_bus;
    esp_codec_dev_handle_t codec;
    uint32_t sample_rate;
    uint32_t source_sample_rate;
    bool initialized;
} s_audio;

static int16_t s_resample_buf[AUDIO_MAX_OUTPUT_FRAMES * 2];

static size_t audio_resample_stereo(const int16_t *input, size_t in_frames,
                                    int16_t *output, size_t out_capacity_frames)
{
    if (in_frames == 0) {
        return 0;
    }

    if (s_audio.source_sample_rate == s_audio.sample_rate) {
        size_t frames_to_copy = in_frames;
        if (frames_to_copy > out_capacity_frames) {
            frames_to_copy = out_capacity_frames;
        }
        memcpy(output, input, frames_to_copy * 2 * sizeof(int16_t));
        return frames_to_copy;
    }

    uint64_t out_frames_u64 = ((uint64_t)in_frames * s_audio.sample_rate +
                               s_audio.source_sample_rate - 1) /
                              s_audio.source_sample_rate;
    size_t out_frames = (size_t)out_frames_u64;
    if (out_frames > out_capacity_frames) {
        out_frames = out_capacity_frames;
    }

    if (in_frames == 1) {
        for (size_t index = 0; index < out_frames; index++) {
            output[index * 2] = input[0];
            output[index * 2 + 1] = input[1];
        }
        return out_frames;
    }

    for (size_t out_index = 0; out_index < out_frames; out_index++) {
        uint64_t src_pos_q16 = (((uint64_t)out_index * s_audio.source_sample_rate) << 16) /
                               s_audio.sample_rate;
        size_t src_index = (size_t)(src_pos_q16 >> 16);
        uint32_t frac = (uint32_t)(src_pos_q16 & 0xFFFFu);

        if (src_index >= (in_frames - 1)) {
            src_index = in_frames - 1;
            frac = 0;
        }

        int32_t left0 = input[src_index * 2];
        int32_t right0 = input[src_index * 2 + 1];
        int32_t left1 = input[(src_index + (src_index + 1 < in_frames ? 1 : 0)) * 2];
        int32_t right1 = input[(src_index + (src_index + 1 < in_frames ? 1 : 0)) * 2 + 1];

        output[out_index * 2] = (int16_t)(left0 + (((left1 - left0) * (int32_t)frac) >> 16));
        output[out_index * 2 + 1] = (int16_t)(right0 + (((right1 - right0) * (int32_t)frac) >> 16));
    }

    return out_frames;
}

static esp_err_t audio_i2c_init(void)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_I2C_SDA_GPIO,
        .scl_io_num = AUDIO_I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&i2c_bus_cfg, &s_audio.i2c_bus);
}

static esp_err_t audio_i2s_init(uint32_t sample_rate)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = AUDIO_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = AUDIO_DMA_FRAME_NUM;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_audio.tx_chan, &s_audio.rx_chan), TAG,
                        "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_GPIO,
            .bclk = AUDIO_I2S_BCLK_GPIO,
            .ws = AUDIO_I2S_WS_GPIO,
            .dout = AUDIO_I2S_DOUT_GPIO,
            .din = AUDIO_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = (i2s_mclk_multiple_t)AUDIO_MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_audio.tx_chan, &std_cfg), TAG,
                        "TX std mode init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_audio.rx_chan, &std_cfg), TAG,
                        "RX std mode init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_audio.tx_chan), TAG, "TX enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_audio.rx_chan), TAG, "RX enable failed");

    return ESP_OK;
}

static esp_err_t audio_codec_init(uint32_t sample_rate)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_audio.i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if != NULL, ESP_FAIL, TAG, "create I2C ctrl failed");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = 0,
        .rx_handle = s_audio.rx_chan,
        .tx_handle = s_audio.tx_chan,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if != NULL, ESP_FAIL, TAG, "create I2S data failed");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if != NULL, ESP_FAIL, TAG, "create GPIO if failed");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = AUDIO_PA_GPIO,
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = AUDIO_MCLK_MULTIPLE,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(codec_if != NULL, ESP_FAIL, TAG, "create ES8311 codec failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_audio.codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_audio.codec != NULL, ESP_FAIL, TAG, "create codec device failed");

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = sample_rate,
    };

    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_audio.codec, &sample_cfg) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "open codec device failed");
    return ESP_OK;
}

esp_err_t audio_driver_init(const audio_driver_config_t *config)
{
    if (config == NULL || config->sample_rate == 0 || config->source_sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_audio.sample_rate = config->sample_rate;
    s_audio.source_sample_rate = config->source_sample_rate;

    esp_err_t err = audio_i2c_init();
    if (err != ESP_OK) {
        return err;
    }

    err = audio_i2s_init(config->sample_rate);
    if (err != ESP_OK) {
        audio_driver_deinit();
        return err;
    }

    err = audio_codec_init(config->sample_rate);
    if (err != ESP_OK) {
        audio_driver_deinit();
        return err;
    }

    s_audio.initialized = true;
    ESP_LOGI(TAG, "Audio driver initialized: src=%lu Hz -> out=%lu Hz stereo",
             (unsigned long)config->source_sample_rate,
             (unsigned long)config->sample_rate);
    return ESP_OK;
}

esp_err_t audio_driver_write(const int16_t *samples, size_t count)
{
    if (!s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t output_frames = audio_resample_stereo(samples, count,
                                                 s_resample_buf,
                                                 AUDIO_MAX_OUTPUT_FRAMES);
    if (output_frames == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t bytes_written = 0;
    size_t bytes_to_write = output_frames * 2 * sizeof(int16_t);
    return i2s_channel_write(s_audio.tx_chan, s_resample_buf, bytes_to_write,
                             &bytes_written, portMAX_DELAY);
}

esp_err_t audio_driver_set_volume(int volume_percent)
{
    if (!s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;

    return (esp_codec_dev_set_out_vol(s_audio.codec, volume_percent) == ESP_CODEC_DEV_OK)
               ? ESP_OK
               : ESP_FAIL;
}

void audio_driver_deinit(void)
{
    if (s_audio.codec) {
        esp_codec_dev_close(s_audio.codec);
        s_audio.codec = NULL;
    }

    if (s_audio.tx_chan) {
        i2s_channel_disable(s_audio.tx_chan);
        i2s_del_channel(s_audio.tx_chan);
        s_audio.tx_chan = NULL;
    }

    if (s_audio.rx_chan) {
        i2s_channel_disable(s_audio.rx_chan);
        i2s_del_channel(s_audio.rx_chan);
        s_audio.rx_chan = NULL;
    }

    if (s_audio.i2c_bus) {
        i2c_del_master_bus(s_audio.i2c_bus);
        s_audio.i2c_bus = NULL;
    }

    s_audio.sample_rate = 0;
    s_audio.source_sample_rate = 0;
    s_audio.initialized = false;
}
