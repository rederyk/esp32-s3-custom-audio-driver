// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#include "i2s_driver.h"

#include "driver/i2s.h"
#include "esp_err.h"
#include "logger.h"

void I2sDriver::configure(uint32_t sample_rate,
                          const AudioConfig &cfg,
                          uint32_t bytes_per_sample,
                          uint32_t channels) {
    uint32_t buf_len = cfg.i2s_dma_buf_len;
    uint32_t buf_count = cfg.i2s_dma_buf_count;

    // Adjust for lower sample rates to keep latency tight.
    if (sample_rate <= 24000) {
        buf_len = 192;
        buf_count = 10;
    } else if (sample_rate >= 48000) {
        buf_len = 256;
        buf_count = 12;
    }

    const uint32_t align = 64 / (bytes_per_sample * channels);
    buf_len = (buf_len + align - 1) / align * align;

    dma_buf_len_active_ = buf_len;
    dma_buf_count_active_ = buf_count;

    size_t dma_bytes = dma_buf_len_active_ * channels * bytes_per_sample;
    chunk_bytes_active_ = dma_bytes * 2;
    if (cfg.i2s_chunk_bytes > 0 && cfg.i2s_chunk_bytes < chunk_bytes_active_) {
        chunk_bytes_active_ = cfg.i2s_chunk_bytes;
    }

    LOG_INFO("I2S tuning: sr=%u -> dma len %u, count %u, chunk %u bytes",
             sample_rate,
             (unsigned)dma_buf_len_active_,
             (unsigned)dma_buf_count_active_,
             (unsigned)chunk_bytes_active_);
}

void I2sDriver::init(uint32_t sample_rate,
                     const AudioConfig &cfg,
                     uint32_t bytes_per_sample,
                     uint32_t channels,
                     int bck_pin,
                     int ws_pin,
                     int dout_pin) {
    configure(sample_rate, cfg, bytes_per_sample, channels);

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = sample_rate,
        .bits_per_sample = static_cast<i2s_bits_per_sample_t>(bytes_per_sample * 8),
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = static_cast<int>(dma_buf_count_active_),
        .dma_buf_len = static_cast<int>(dma_buf_len_active_),
        .use_apll = cfg.i2s_use_apll};

    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = bck_pin,
        .ws_io_num = ws_pin,
        .data_out_num = dout_pin,
        .data_in_num = I2S_PIN_NO_CHANGE};

    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pin_config));

    installed_ = true;

    LOG_INFO("Driver I2S installato per %d Hz, %u-bit, Stereo (dma len %u, count %u, chunk %u bytes)",
             sample_rate,
             (unsigned)(bytes_per_sample * 8),
             (unsigned)dma_buf_len_active_,
             (unsigned)dma_buf_count_active_,
             (unsigned)chunk_bytes_active_);
}

void I2sDriver::uninstall() {
    if (!installed_) {
        return;
    }
    i2s_driver_uninstall(I2S_NUM_0);
    installed_ = false;
}
