// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include "audio_decoder.h"
#include "mp3_decoder.h"

// Adapter che wrappa Mp3Decoder esistente per implementare IAudioDecoder
class Mp3DecoderAdapter : public IAudioDecoder {
public:
    Mp3DecoderAdapter() = default;
    ~Mp3DecoderAdapter() override = default;

    bool init(IDataSource* source, size_t frames_per_chunk, bool build_seek_table = true) override {
        return decoder_.init(source, frames_per_chunk, build_seek_table);
    }

    void shutdown() override {
        decoder_.shutdown();
    }

    uint64_t read_frames(int16_t* dst, uint64_t frames) override {
        return decoder_.read_frames(dst, frames);
    }

    bool seek_to_frame(uint64_t frame_index) override {
        return decoder_.seek_to_frame(frame_index);
    }

    uint32_t sample_rate() const override {
        return decoder_.sample_rate();
    }

    uint32_t channels() const override {
        return decoder_.channels();
    }

    uint64_t total_frames() const override {
        return decoder_.total_frames();
    }

    bool initialized() const override {
        return decoder_.initialized();
    }

    AudioFormat format() const override {
        return AudioFormat::MP3;
    }

    uint32_t bitrate() const override {
        return decoder_.bitrate();
    }

    bool has_seek_table() const override {
        return decoder_.has_seek_table();
    }

    // Accesso diretto al decoder nativo per compatibilità (se necessario)
    Mp3Decoder& native_decoder() {
        return decoder_;
    }

    const Mp3Decoder& native_decoder() const {
        return decoder_;
    }

private:
    Mp3Decoder decoder_;
};
