// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include "audio_decoder.h"
#include "data_source.h"
#include <cstdint>

// Decoder per file WAV PCM (non compresso)
// Supporta solo WAV PCM 16-bit stereo/mono
class WavDecoder : public IAudioDecoder {
public:
    WavDecoder() = default;
    ~WavDecoder() override;

    bool init(IDataSource* source, size_t frames_per_chunk, bool build_seek_table = true) override;
    void shutdown() override;

    uint64_t read_frames(int16_t* dst, uint64_t frames) override;
    bool seek_to_frame(uint64_t frame_index) override;

    uint32_t sample_rate() const override { return sample_rate_; }
    uint32_t channels() const override { return channels_; }
    uint64_t total_frames() const override { return total_frames_; }
    bool initialized() const override { return initialized_; }
    AudioFormat format() const override { return AudioFormat::WAV; }
    uint32_t bitrate() const override;
    bool has_seek_table() const override { return true; } // WAV supporta seek immediato

private:
    bool parse_wav_header();

    IDataSource* source_ = nullptr;
    bool initialized_ = false;
    uint32_t sample_rate_ = 0;
    uint32_t channels_ = 0;
    uint16_t bits_per_sample_ = 0;
    uint64_t total_frames_ = 0;
    size_t data_offset_ = 0;      // Offset dei dati PCM nel file
    size_t data_size_ = 0;        // Dimensione dei dati PCM in bytes
    uint64_t current_frame_ = 0;  // Frame corrente di playback
};
