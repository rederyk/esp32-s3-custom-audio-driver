// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.

#include "audio_output.h"
#include "logger.h"
#include "driver/i2s.h"

namespace {
// --- Pinout Configuration ---
constexpr int kI2sBck = 5;
constexpr int kI2sDout = 8;
constexpr int kI2sWs = 7;
constexpr int kApEnable = 1;
constexpr int kI2cScl = 15;
constexpr int kI2cSda = 16;
constexpr uint32_t kI2cSpeed = 400000;
constexpr uint32_t kBytesPerSample = sizeof(int16_t);
} // namespace

AudioOutput::AudioOutput() {
}

AudioOutput::~AudioOutput() {
    end();
}

bool AudioOutput::begin(const AudioConfig& cfg, uint32_t sample_rate, uint32_t channels) {
    i2s_write_timeout_ms_ = cfg.i2s_write_timeout_ms;
    current_sample_rate_ = sample_rate;

    // ===== INIT CODEC =====
    // Note: Codec init requires I2C.
    if (!codec_.init(sample_rate, kApEnable, kI2cSda, kI2cScl, kI2cSpeed, cfg.default_volume_percent)) {
        LOG_ERROR("Codec init failed");
        return false;
    }

    // ===== INIT I2S =====
    // I2sDriver handles the specific ESP32 I2S configuration
    i2s_driver_.init(sample_rate, cfg, kBytesPerSample, channels, kI2sBck, kI2sWs, kI2sDout);
    
    if (!i2s_driver_.installed()) {
        LOG_ERROR("I2S driver init failed");
        return false;
    }

    initialized_ = true;
    return true;
}

void AudioOutput::end() {
    if (initialized_) {
        i2s_driver_.uninstall();
        initialized_ = false;
    }
}

void AudioOutput::stop() {
    if (initialized_) {
        i2s_zero_dma_buffer(I2S_NUM_0);
    }
}

size_t AudioOutput::write(const int16_t* data, size_t frames, size_t channels) {
    if (!initialized_) return 0;

    size_t pcm_bytes = frames * channels * sizeof(int16_t);
    const uint8_t* write_ptr = reinterpret_cast<const uint8_t*>(data);
    size_t remaining = pcm_bytes;
    size_t total_written_bytes = 0;

    while (remaining > 0) {
        size_t chunk = remaining;
        if (chunk > i2s_driver_.chunk_bytes()) {
            chunk = i2s_driver_.chunk_bytes();
        }

        size_t written = 0;
        esp_err_t result = i2s_write(I2S_NUM_0,
                                    write_ptr,
                                    chunk,
                                    &written,
                                    pdMS_TO_TICKS(i2s_write_timeout_ms_));

        if (result != ESP_OK) {
            LOG_ERROR("I2S write error: %s", esp_err_to_name(result));
            break;
        }

        if (written == 0) {
            LOG_ERROR("I2S write returned 0 bytes");
            break;
        }

        remaining -= written;
        write_ptr += written;
        total_written_bytes += written;
    }

    return total_written_bytes / (channels * sizeof(int16_t));
}

void AudioOutput::set_volume(int percent) {
    codec_.set_volume(percent);
}
