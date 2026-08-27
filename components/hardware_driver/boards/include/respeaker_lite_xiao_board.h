#pragma once

#include "driver/gpio.h"
#include "esp_idf_version.h"

/* ReSpeaker Lite + XIAO ESP32-S3 */

/* No external codec control needed for WakeNet input */
#define FUNC_I2C_EN        (0)
#define GPIO_I2C_SCL       (GPIO_NUM_NC)
#define GPIO_I2C_SDA       (GPIO_NUM_NC)

/* No SD card used by SAT-001 */
#define FUNC_SDMMC_EN      (0)
#define SDMMC_BUS_WIDTH    (1)
#define GPIO_SDMMC_CLK     (GPIO_NUM_NC)
#define GPIO_SDMMC_CMD     (GPIO_NUM_NC)
#define GPIO_SDMMC_D0      (GPIO_NUM_NC)
#define GPIO_SDMMC_D1      (GPIO_NUM_NC)
#define GPIO_SDMMC_D2      (GPIO_NUM_NC)
#define GPIO_SDMMC_D3      (GPIO_NUM_NC)
#define GPIO_SDMMC_DET     (GPIO_NUM_NC)

#define FUNC_SDSPI_EN      (0)
#define SDSPI_HOST         (SPI2_HOST)
#define GPIO_SDSPI_CS      (GPIO_NUM_NC)
#define GPIO_SDSPI_SCLK    (GPIO_NUM_NC)
#define GPIO_SDSPI_MISO    (GPIO_NUM_NC)
#define GPIO_SDSPI_MOSI    (GPIO_NUM_NC)

/*
 * XIAO <-> ReSpeaker Lite XU316 I2S
 * XU316 supplies BCLK/LRCLK; ESP32-S3 operates as I2S slave.
 */
#define FUNC_I2S_EN        (1)
#define GPIO_I2S_LRCK      (GPIO_NUM_7)
#define GPIO_I2S_MCLK      (GPIO_NUM_NC)
#define GPIO_I2S_SCLK      (GPIO_NUM_8)
#define GPIO_I2S_SDIN      (GPIO_NUM_44)
#define GPIO_I2S_DOUT      (GPIO_NUM_43)

#define FUNC_I2S0_EN       (0)
#define GPIO_I2S0_LRCK     (GPIO_NUM_NC)
#define GPIO_I2S0_MCLK     (GPIO_NUM_NC)
#define GPIO_I2S0_SCLK     (GPIO_NUM_NC)
#define GPIO_I2S0_SDIN     (GPIO_NUM_NC)
#define GPIO_I2S0_DOUT     (GPIO_NUM_NC)

#define FUNC_PWR_CTRL      (0)
#define GPIO_PWR_CTRL      (GPIO_NUM_NC)
#define GPIO_PWR_ON_LEVEL  (1)

#define I2S_CONFIG_DEFAULT(sample_rate, channel_fmt, bits_per_chan) { \
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate), \
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits_per_chan, channel_fmt), \
    .gpio_cfg = { \
        .mclk = GPIO_I2S_MCLK, \
        .bclk = GPIO_I2S_SCLK, \
        .ws   = GPIO_I2S_LRCK, \
        .dout = GPIO_I2S_DOUT, \
        .din  = GPIO_I2S_SDIN, \
        .invert_flags = { \
            .mclk_inv = false, \
            .bclk_inv = false, \
            .ws_inv   = false, \
        }, \
    }, \
}
