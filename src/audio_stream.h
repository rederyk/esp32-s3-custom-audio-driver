// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include <memory>
#include <cstdint>
#include <cstddef>
#include "data_source.h"
#include "audio_decoder.h"

class AudioStream {
public:
    AudioStream();
    ~AudioStream();

    // Takes ownership of the data source and auto-detects format
    bool begin(std::unique_ptr<IDataSource> source);

    // Takes ownership and uses explicit format
    bool begin(std::unique_ptr<IDataSource> source, AudioFormat format);

    void end();

    // Reads PCM samples into buffer. Returns number of frames read.
    size_t read(int16_t* buffer, size_t frames_to_read);

    // Seeks to specific PCM frame index. Returns true on success.
    bool seek(uint64_t pcm_frame_index);

    // Getters
    uint32_t sample_rate() const;
    uint32_t channels() const;
    uint64_t total_frames() const;
    AudioFormat format() const;
    uint32_t bitrate() const;

    // Access underlying data source
    const IDataSource* data_source() const { return source_.get(); }

private:
    std::unique_ptr<IDataSource> source_;
    std::unique_ptr<IAudioDecoder> decoder_;  // Polymorphic decoder!
    bool initialized_ = false;
};
