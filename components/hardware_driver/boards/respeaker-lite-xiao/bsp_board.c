#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_board.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "respeaker-lite-xiao";
static i2s_chan_handle_t rx_handle = NULL;

static int32_t *raw_buffer = NULL;
static size_t raw_capacity = 0;

esp_err_t bsp_board_init(uint32_t sample_rate, int channel_format, int bits_per_chan)
{
    (void)sample_rate;
    (void)channel_format;
    (void)bits_per_chan;

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg =
        I2S_CONFIG_DEFAULT(16000, I2S_SLOT_MODE_STEREO, 32);

    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG,
             "SAT-001 I2S ready: slave, 16kHz, stereo, 32-bit, BCLK=8 WS=7 DIN=44");

    return ESP_OK;
}

esp_err_t bsp_get_feed_data(bool is_get_raw_channel,
                            int16_t *buffer,
                            int buffer_len)
{
    (void)is_get_raw_channel;

    if (!rx_handle || !buffer || buffer_len <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * AFE wants signed 16-bit interleaved samples.
     * ReSpeaker/XU316 supplies signed 32-bit stereo samples.
     */
    size_t output_samples = buffer_len / sizeof(int16_t);
    size_t raw_bytes = output_samples * sizeof(int32_t);

    if (raw_bytes > raw_capacity) {
        int32_t *new_buffer = realloc(raw_buffer, raw_bytes);
        if (!new_buffer) {
            return ESP_ERR_NO_MEM;
        }

        raw_buffer = new_buffer;
        raw_capacity = raw_bytes;
    }

    size_t bytes_read = 0;

    esp_err_t ret = i2s_channel_read(
        rx_handle,
        raw_buffer,
        raw_bytes,
        &bytes_read,
        portMAX_DELAY
    );

    if (ret != ESP_OK) {
        return ret;
    }

    size_t samples_read = bytes_read / sizeof(int32_t);

    if (samples_read > output_samples) {
        samples_read = output_samples;
    }

    for (size_t i = 0; i < samples_read; i++) {
        buffer[i] = (int16_t)(raw_buffer[i] >> 16);
    }

    if (samples_read < output_samples) {
        memset(
            &buffer[samples_read],
            0,
            (output_samples - samples_read) * sizeof(int16_t)
        );
    }

    return ESP_OK;
}

int bsp_get_feed_channel(void)
{
    return 2;
}

char *bsp_get_input_format(void)
{
    return "MM";
}

esp_err_t bsp_audio_play(const int16_t *data,
                         int length,
                         TickType_t ticks_to_wait)
{
    (void)data;
    (void)length;
    (void)ticks_to_wait;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_audio_set_play_vol(int volume)
{
    (void)volume;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_audio_get_play_vol(int *volume)
{
    (void)volume;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_sdcard_init(char *mount_point, size_t max_files)
{
    (void)mount_point;
    (void)max_files;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_sdcard_deinit(char *mount_point)
{
    (void)mount_point;
    return ESP_ERR_NOT_SUPPORTED;
}
