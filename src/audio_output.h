// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include <cstdint>
#include <cstddef>
#include "audio_types.h"
#include "codec_es8311.h"
#include "i2s_driver.h"

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    bool begin(const AudioConfig& cfg, uint32_t sample_rate, uint32_t channels);
    void end();
    void stop(); // Clears DMA buffers
    size_t write(const int16_t* data, size_t frames, size_t channels);
    void set_volume(int percent);
    
    // Metodi di utilità
    size_t chunk_bytes() const { return i2s_driver_.chunk_bytes(); }

private:
    CodecES8311 codec_;
    I2sDriver i2s_driver_;
    bool initialized_ = false;
    uint32_t current_sample_rate_ = 0;
    uint32_t i2s_write_timeout_ms_ = 0;
};
