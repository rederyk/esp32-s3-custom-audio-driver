// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include <cstddef>
#include <cstdint>
#include "dr_mp3.h"
#include "data_source.h"
#include "mp3_seek_table.h"

class Mp3Decoder {
public:
    struct Buffers {
        int16_t *pcm = nullptr;
        size_t pcm_capacity_frames = 0;
    };

    Mp3Decoder() = default;
    ~Mp3Decoder();

    bool init(IDataSource* source, size_t frames_per_chunk, bool build_seek_table = true);
    drmp3_uint64 read_frames(int16_t *dst, drmp3_uint64 frames);
    bool seek_to_frame(drmp3_uint64 frame_index);
    void shutdown();

    bool has_seek_table() const { return seek_table_.is_ready(); }

    uint32_t sample_rate() const { return mp3_ ? mp3_->sampleRate : 0; }
    uint32_t channels() const { return mp3_ ? mp3_->channels : 0; }
    drmp3_uint64 total_frames() const;
    uint32_t bitrate() const;  // Bitrate in kbps
    Buffers &buffers() { return buffers_; }
    bool initialized() const { return initialized_; }
    drmp3* mp3() { return mp3_; }

private:
    static size_t on_read_cb(void *user, void *buffer, size_t bytesToRead);
    static drmp3_bool32 on_seek_cb(void *user, int offset, drmp3_seek_origin origin);
    static drmp3_bool32 on_tell_cb(void *user, drmp3_int64 *pCursor);

    size_t do_read(void* buffer, size_t bytes_to_read);
    bool do_seek(int offset, drmp3_seek_origin origin);
    drmp3_int64 do_tell();
    bool ensure_buffers(size_t pcm_frames);
    bool reinit_decoder();
    drmp3_seek_proc current_seek_cb() const;
    drmp3_tell_proc current_tell_cb() const;

    IDataSource* source_ = nullptr;
    drmp3 *mp3_ = nullptr;
    Buffers buffers_;
    bool initialized_ = false;
    Mp3SeekTable seek_table_;
    uint8_t* mp3_file_cache_ = nullptr;  // Cache del file MP3 per seek table
    size_t mp3_file_size_ = 0;
    size_t stream_base_offset_ = 0;      // Offset di base usato come "inizio" logico per dr_mp3
    size_t stream_size_ = 0;             // Cache della size() della sorgente per SEEK_END
};
