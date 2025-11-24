#pragma once

#include <cstddef>
#include <cstdint>
#include "dr_mp3.h"

class AudioPlayer;

class Mp3Decoder {
public:
    struct Buffers {
        int16_t *pcm = nullptr;
        size_t pcm_capacity_frames = 0;
        uint8_t *leftover = nullptr;
        size_t leftover_capacity = 0;
    };

    Mp3Decoder() = default;
    ~Mp3Decoder();

    bool init(AudioPlayer *owner, size_t frames_per_chunk, size_t leftover_capacity);
    drmp3_uint64 read_frames(int16_t *dst, drmp3_uint64 frames);
    void shutdown();

    uint32_t sample_rate() const { return mp3_ ? mp3_->sampleRate : 0; }
    uint32_t channels() const { return mp3_ ? mp3_->channels : 0; }
    drmp3_uint64 total_frames() const;
    Buffers &buffers() { return buffers_; }
    bool initialized() const { return initialized_; }

private:
    static size_t on_read_cb(void *user, void *buffer, size_t bytesToRead);
    size_t fill_buffer(uint8_t *out, size_t bytes_to_read);
    bool ensure_buffers(size_t pcm_frames, size_t leftover_bytes);

    AudioPlayer *owner_ = nullptr;
    drmp3 *mp3_ = nullptr;
    Buffers buffers_;
    size_t leftover_size_ = 0;
    size_t leftover_offset_ = 0;
    bool initialized_ = false;
};
