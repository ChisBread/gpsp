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

/* Fixed-ratio resampler: source (65536 Hz) → output (e.g. 64000 Hz).
 * Max output frames per call: input can be up to ~1100 frames/call,
 * and ratio ≤ 1 so output ≤ input. Add margin for rounding. */
#define AUDIO_RESAMPLE_BUF_FRAMES  1200

static __attribute__((section(".ext_ram.bss"))) struct {
    i2s_chan_handle_t tx_chan;
    i2c_master_bus_handle_t i2c_bus;
    esp_codec_dev_handle_t codec;
    uint32_t sample_rate;           /* I2S output sample rate */
    uint32_t sample_rate_nominal;
    uint32_t resample_step_q16;     /* fixed Q16.16 step = src_rate/out_rate */
    uint32_t resample_phase_q16;    /* accumulator across calls */
    int16_t  prev_sample[2];        /* last input sample for interpolation */
    bool     prev_valid;
    bool     resample_needed;       /* true when src_rate != out_rate */
    bool     initialized;
} s_audio;

static __attribute__((section(".ext_ram.bss"))) int16_t s_resample_buf[AUDIO_RESAMPLE_BUF_FRAMES * 2];

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

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_audio.tx_chan, NULL), TAG,
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
            .din = I2S_GPIO_UNUSED,
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
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_audio.tx_chan), TAG, "TX enable failed");

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
        .rx_handle = NULL,
        .tx_handle = s_audio.tx_chan,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if != NULL, ESP_FAIL, TAG, "create I2S data failed");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if != NULL, ESP_FAIL, TAG, "create GPIO if failed");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_TYPE_OUT,
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = AUDIO_PA_GPIO,
        .pa_reverted = false,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = AUDIO_MCLK_MULTIPLE,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(codec_if != NULL, ESP_FAIL, TAG, "create ES8311 codec failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
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
    if (config == NULL || config->sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_audio.sample_rate = config->sample_rate;
    s_audio.sample_rate_nominal = config->sample_rate;

    /* Fixed resample ratio: AUDIO_PCM_SOURCE_RATE → sample_rate.
     * Shrink step by ~0.3% so the resampler slightly over-produces,
     * keeping I2S DMA fed and providing gentle backpressure that
     * prevents underruns from async timing jitter. */
    if (AUDIO_PCM_SOURCE_RATE != config->sample_rate) {
        uint32_t nominal = (uint32_t)(((uint64_t)AUDIO_PCM_SOURCE_RATE << 16)
                                       / config->sample_rate);
        s_audio.resample_step_q16 = nominal;
        s_audio.resample_needed = true;
    } else {
        s_audio.resample_step_q16 = (1u << 16);
        s_audio.resample_needed = false;
    }
    s_audio.resample_phase_q16 = 0;
    s_audio.prev_sample[0] = 0;
    s_audio.prev_sample[1] = 0;
    s_audio.prev_valid = false;

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
    ESP_LOGI(TAG, "Audio driver initialized: I2S output %lu Hz stereo",
             (unsigned long)config->sample_rate);
    return ESP_OK;
}

/* Fixed-ratio linear-interpolation resampler (e.g. 65536 → 64000).
 * Operates on interleaved stereo.  Carries phase and last sample
 * across calls for seamless output. */
static size_t audio_resample(const int16_t *in, size_t in_frames,
                             int16_t *out, size_t out_cap)
{
    if (in_frames == 0) return 0;

    uint32_t phase = s_audio.resample_phase_q16;
    const uint32_t step = s_audio.resample_step_q16;
    size_t out_frames = 0;

    /* Extended source: index 0 = prev_sample (if valid), then input[]. */
    const size_t ext_len = in_frames + (s_audio.prev_valid ? 1u : 0u);

    #define EXT_L(i) ( (s_audio.prev_valid) \
        ? ((i) == 0 ? s_audio.prev_sample[0] : in[((i)-1)*2])   \
        : in[(i)*2] )
    #define EXT_R(i) ( (s_audio.prev_valid) \
        ? ((i) == 0 ? s_audio.prev_sample[1] : in[((i)-1)*2+1]) \
        : in[(i)*2+1] )

    const uint32_t max_phase = (uint32_t)((ext_len - 1) << 16);

    while (out_frames < out_cap && phase < max_phase) {
        size_t idx = phase >> 16;
        uint32_t frac = (phase >> 1) & 0x7FFFu;  /* 15-bit frac to avoid int32 overflow */
        int32_t l0 = EXT_L(idx), r0 = EXT_R(idx);
        int32_t l1 = EXT_L(idx+1), r1 = EXT_R(idx+1);
        out[out_frames*2]     = (int16_t)(l0 + (((l1 - l0) * (int32_t)frac) >> 15));
        out[out_frames*2 + 1] = (int16_t)(r0 + (((r1 - r0) * (int32_t)frac) >> 15));
        out_frames++;
        phase += step;
    }

    #undef EXT_L
    #undef EXT_R

    /* Save tail for next call. */
    s_audio.prev_sample[0] = in[(in_frames - 1) * 2];
    s_audio.prev_sample[1] = in[(in_frames - 1) * 2 + 1];
    s_audio.prev_valid = true;

    /* Carry residual phase. */
    s_audio.resample_phase_q16 = phase - max_phase;

    return out_frames;
}

esp_err_t audio_driver_write(const int16_t *samples, size_t count)
{
    if (!s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t bytes_written = 0;

    if (!s_audio.resample_needed) {
        size_t bytes_to_write = count * 2 * sizeof(int16_t);
        return i2s_channel_write(s_audio.tx_chan, samples, bytes_to_write,
                                 &bytes_written, portMAX_DELAY);
    }

    size_t out_frames = audio_resample(samples, count,
                                       s_resample_buf,
                                       AUDIO_RESAMPLE_BUF_FRAMES);
    if (out_frames == 0) {
        return ESP_OK;
    }

    size_t bytes_to_write = out_frames * 2 * sizeof(int16_t);
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


uint32_t audio_driver_get_output_rate(void)
{
    return s_audio.sample_rate;
}

uint32_t audio_driver_get_nominal_rate(void)
{
    return s_audio.sample_rate_nominal;
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
    if (s_audio.i2c_bus) {
        i2c_del_master_bus(s_audio.i2c_bus);
        s_audio.i2c_bus = NULL;
    }

    s_audio.sample_rate = 0;
    s_audio.sample_rate_nominal = 0;
    s_audio.resample_step_q16 = 0;
    s_audio.resample_phase_q16 = 0;
    s_audio.prev_sample[0] = 0;
    s_audio.prev_sample[1] = 0;
    s_audio.prev_valid = false;
    s_audio.resample_needed = false;
    s_audio.initialized = false;
}
