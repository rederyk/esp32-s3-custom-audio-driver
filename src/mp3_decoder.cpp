// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#define DR_MP3_IMPLEMENTATION
#include "mp3_decoder.h"

#include <Arduino.h>
#include <cstring>
#include <esp_heap_caps.h>
#include "logger.h"

namespace {
constexpr uint32_t kBytesPerSample = sizeof(int16_t);
constexpr uint32_t kDefaultChannels = 2;
}

Mp3Decoder::~Mp3Decoder() {
    shutdown();
}

bool Mp3Decoder::ensure_buffers(size_t pcm_frames) {
    size_t pcm_bytes = pcm_frames * kDefaultChannels * kBytesPerSample;

    if (buffers_.pcm == nullptr || buffers_.pcm_capacity_frames < pcm_frames) {
        int16_t *new_pcm = static_cast<int16_t *>(heap_caps_realloc(buffers_.pcm, pcm_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!new_pcm) {
            return false;
        }
        buffers_.pcm = new_pcm;
        buffers_.pcm_capacity_frames = pcm_frames;
    }

    return true;
}

bool Mp3Decoder::init(IDataSource* source, size_t frames_per_chunk, bool build_seek_table) {
    if (!source || !source->is_open()) {
        LOG_ERROR("DataSource not available or not open");
        return false;
    }

    source_ = source;
    stream_base_offset_ = 0;
    stream_size_ = source_->size();

    if (!ensure_buffers(frames_per_chunk)) {
        LOG_ERROR("Failed to allocate PCM buffer (%u frames)", static_cast<unsigned>(frames_per_chunk));
        return false;
    }

    mp3_ = static_cast<drmp3 *>(heap_caps_calloc(1, sizeof(drmp3), MALLOC_CAP_SPIRAM));
    if (!mp3_) {
        LOG_ERROR("Failed to allocate drmp3 struct");
        return false;
    }

    // Inizializza dr_mp3 con callbacks per read, seek e tell (solo se la sorgente è seekable)
    if (!drmp3_init(mp3_, on_read_cb, current_seek_cb(), current_tell_cb(), NULL, this, NULL)) {
        LOG_ERROR("Failed to initialize dr_mp3");
        heap_caps_free(mp3_);
        mp3_ = nullptr;
        return false;
    }

    initialized_ = true;

    LOG_INFO("Mp3Decoder initialized: %u Hz, %u ch, seekable=%s",
             sample_rate(), channels(),
             source_->is_seekable() ? "yes" : "no");

    // Build seek table se richiesto e source è seekable
    if (build_seek_table && source_->is_seekable() && source_->size() > 0) {
        mp3_file_size_ = source_->size();

        // Alloca cache per file MP3 (solo se non troppo grande)
        if (mp3_file_size_ < 10 * 1024 * 1024) {  // Max 10MB
            mp3_file_cache_ = static_cast<uint8_t*>(
                heap_caps_malloc(mp3_file_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
            );

            if (mp3_file_cache_) {
                // Leggi file intero in cache
                source_->seek(0);
                size_t read = source_->read(mp3_file_cache_, mp3_file_size_);

                if (read == mp3_file_size_) {
                    // Build seek table
                    uint32_t frames_per_entry = sample_rate() / 10;  // Entry ogni 100ms
                    if (seek_table_.build(mp3_file_cache_, mp3_file_size_, sample_rate(), frames_per_entry)) {
                        LOG_INFO("Seek table ready: %u entries (%u KB)",
                                 (unsigned)seek_table_.size(),
                                 (unsigned)(seek_table_.memory_bytes() / 1024));
                    } else {
                        LOG_WARN("Failed to build seek table");
                    }
                } else {
                    LOG_WARN("Failed to read MP3 file for seek table");
                }

                // Torna all'inizio dopo il build
                source_->seek(0);
                drmp3_seek_to_pcm_frame(mp3_, 0);
            } else {
                LOG_WARN("Not enough memory for MP3 cache (%u bytes), skip seek table",
                         (unsigned)mp3_file_size_);
            }
        } else {
            LOG_INFO("File too large for seek table (%u bytes), using dr_mp3 seek",
                     (unsigned)mp3_file_size_);
        }
    }

    return true;
}

void Mp3Decoder::shutdown() {
    if (mp3_ && initialized_) {
        drmp3_uninit(mp3_);
    }
    if (mp3_) {
        heap_caps_free(mp3_);
        mp3_ = nullptr;
    }
    if (buffers_.pcm) {
        heap_caps_free(buffers_.pcm);
        buffers_.pcm = nullptr;
    }
    if (mp3_file_cache_) {
        heap_caps_free(mp3_file_cache_);
        mp3_file_cache_ = nullptr;
    }
    buffers_.pcm_capacity_frames = 0;
    mp3_file_size_ = 0;
    seek_table_.clear();
    source_ = nullptr;
    initialized_ = false;
    stream_base_offset_ = 0;
    stream_size_ = 0;
}

drmp3_uint64 Mp3Decoder::read_frames(int16_t *dst, drmp3_uint64 frames) {
    if (!mp3_ || !initialized_) {
        return 0;
    }
    return drmp3_read_pcm_frames_s16(mp3_, frames, dst);
}

bool Mp3Decoder::seek_to_frame(drmp3_uint64 frame_index) {
    if (!mp3_ || !initialized_) {
        return false;
    }

    if (!source_->is_seekable()) {
        LOG_WARN("DataSource not seekable, cannot perform native seek");
        return false;
    }

    uint32_t seek_start = millis();
    // Always refresh stream_size_ as it might change for live streams (Timeshift)
    stream_size_ = source_->size(); // Update cached size

    // Usa seek table: re-inizializza dr_mp3 facendo credere che l'offset sia l'inizio del file
    
    // Check if source provides a seek table (e.g. TimeshiftManager), otherwise use internal one
    const Mp3SeekTable* table_ptr = source_->get_seek_table();
    bool use_table = (table_ptr && table_ptr->is_ready());
    if (!use_table) {
        table_ptr = &seek_table_;
        use_table = seek_table_.is_ready();
    }

    if (use_table) {
        uint64_t byte_offset = 0;
        uint64_t nearest_frame = 0;

        if (table_ptr->find_seek_point(frame_index, &byte_offset, &nearest_frame) && nearest_frame <= frame_index) {
            stream_base_offset_ = static_cast<size_t>(byte_offset);

            if (!reinit_decoder()) {
                stream_base_offset_ = 0;
                return false;
            }

            uint64_t frames_to_skip = frame_index - nearest_frame;
            
            // Optimization: if skippint is huge (> 5 sec), maybe just use byte seek inaccuracy?
            // But precision is good.

            if (frames_to_skip > 0) {
                int16_t temp_buf[2048];
                uint64_t total_skipped = 0;

                while (total_skipped < frames_to_skip) {
                    uint64_t skip_now = (frames_to_skip - total_skipped > 1024)
                        ? 1024
                        : (frames_to_skip - total_skipped);
                    uint64_t skipped = drmp3_read_pcm_frames_s16(mp3_, skip_now, temp_buf);

                    if (skipped == 0) {
                        LOG_WARN("Unexpected EOF while skipping frames (skipped %llu/%llu)",
                                 total_skipped, frames_to_skip);
                        break;
                    }

                    total_skipped += skipped;
                }

                LOG_DEBUG("Skipped %llu frames to reach target", total_skipped);
            }

            uint32_t seek_end = millis();
            LOG_INFO("SEEK TABLE used: %u ms (target frame=%llu)",
                     seek_end - seek_start, frame_index);
            return true;
        }
        LOG_DEBUG("Seek table does not cover target frame %llu, using dr_mp3 seek", frame_index);
    }

    // Fallback a dr_mp3 seek standard
    stream_base_offset_ = 0;

    if (!reinit_decoder()) {
        return false;
    }

    drmp3_bool32 result = drmp3_seek_to_pcm_frame(mp3_, frame_index);
    uint32_t seek_end = millis();

    size_t current_pos = source_->tell();

    if (result == DRMP3_TRUE) {
        LOG_INFO("dr_mp3 seek: %u ms, file pos -> %u", seek_end - seek_start, (unsigned)current_pos);
        return true;
    } else {
        LOG_ERROR("dr_mp3 seek FAILED after %u ms", seek_end - seek_start);
        return false;
    }
}

drmp3_uint64 Mp3Decoder::total_frames() const {
    if (!mp3_ || !initialized_) {
        return 0;
    }
    return drmp3_get_pcm_frame_count(mp3_);
}

uint32_t Mp3Decoder::bitrate() const {
    if (!mp3_ || !initialized_ || !source_) {
        return 0;
    }

    // Calculate average bitrate: (file_size_bytes * 8) / duration_seconds / 1000
    uint64_t total = total_frames();
    uint32_t sr = sample_rate();

    if (total == 0 || sr == 0) {
        return 0;
    }

    size_t file_size = source_->size();
    if (file_size == 0) {
        return 0;
    }

    // Duration in seconds = total_frames / sample_rate
    // Bitrate (kbps) = (file_size * 8) / duration / 1000
    uint64_t duration_ms = (total * 1000) / sr;
    if (duration_ms == 0) {
        return 0;
    }

    uint64_t bitrate_bps = (file_size * 8 * 1000) / duration_ms;
    return static_cast<uint32_t>(bitrate_bps / 1000);  // Convert to kbps
}

// ========== CALLBACKS dr_mp3 ==========

size_t Mp3Decoder::on_read_cb(void *user, void *buffer, size_t bytesToRead) {
    Mp3Decoder *self = static_cast<Mp3Decoder *>(user);
    return self ? self->do_read(buffer, bytesToRead) : 0;
}

drmp3_bool32 Mp3Decoder::on_seek_cb(void *user, int offset, drmp3_seek_origin origin) {
    Mp3Decoder *self = static_cast<Mp3Decoder *>(user);
    if (!self) {
        return DRMP3_FALSE;
    }
    return self->do_seek(offset, origin) ? DRMP3_TRUE : DRMP3_FALSE;
}

drmp3_bool32 Mp3Decoder::on_tell_cb(void *user, drmp3_int64 *pCursor) {
    Mp3Decoder *self = static_cast<Mp3Decoder *>(user);
    if (!self || !pCursor) {
        return DRMP3_FALSE;
    }
    *pCursor = self->do_tell();
    return DRMP3_TRUE;
}

size_t Mp3Decoder::do_read(void* buffer, size_t bytes_to_read) {
    if (!source_ || !source_->is_open()) {
        return 0;
    }

    // Leggi direttamente da DataSource - NESSUN ring buffer!
    return source_->read(buffer, bytes_to_read);
}

bool Mp3Decoder::do_seek(int offset, drmp3_seek_origin origin) {
    if (!source_ || !source_->is_seekable()) {
        return false;
    }

    size_t current_abs = source_->tell();
    size_t current_pos = (current_abs >= stream_base_offset_) ? (current_abs - stream_base_offset_) : 0;
    size_t target_pos = current_pos;

    switch (origin) {
        case DRMP3_SEEK_SET:
            if (offset < 0) {
                target_pos = 0;
            } else {
                target_pos = static_cast<size_t>(offset);
            }
            break;

        case DRMP3_SEEK_CUR:
            if (offset < 0) {
                size_t delta = static_cast<size_t>(-offset);
                target_pos = (delta > current_pos) ? 0 : (current_pos - delta);
            } else {
                target_pos = current_pos + static_cast<size_t>(offset);
            }
            break;

        case DRMP3_SEEK_END:
            // Seek from end: offset è negativo dalla fine del file
            if (source_->size() == 0 || stream_size_ == 0) {
                LOG_WARN("Cannot seek from end: file size unknown");
                return false;
            }
            {
                size_t effective_size = (stream_size_ > stream_base_offset_) ? (stream_size_ - stream_base_offset_) : 0;
                if (offset < 0) {
                    size_t delta = static_cast<size_t>(-offset);
                    target_pos = (delta > effective_size) ? 0 : (effective_size - delta);
                } else {
                    target_pos = effective_size + static_cast<size_t>(offset);
                }
            }
            break;

        default:
            LOG_WARN("Unsupported seek origin: %d", origin);
            return false;
    }

    // Clamp to stream size if known
    if (stream_size_ > 0) {
        size_t effective_size = (stream_size_ > stream_base_offset_) ? (stream_size_ - stream_base_offset_) : 0;
        if (target_pos > effective_size) {
            target_pos = effective_size;
        }
    }

    // Esegui seek sulla DataSource
    uint32_t seek_start = millis();
    size_t absolute_target = stream_base_offset_ + target_pos;
    bool success = source_->seek(absolute_target);
    uint32_t seek_end = millis();

    static int seek_call_count = 0;
    seek_call_count++;

    if (success) {
        LOG_DEBUG("do_seek #%d: origin=%d, rel %u -> %u (abs %u) (%+d bytes) in %u ms",
                  seek_call_count, (int)origin,
                  (unsigned)current_pos, (unsigned)target_pos,
                  (unsigned)absolute_target,
                  (int)(target_pos - current_pos),
                  seek_end - seek_start);
    } else {
        LOG_ERROR("do_seek #%d FAILED: target byte %u (abs %u)", seek_call_count, (unsigned)target_pos, (unsigned)absolute_target);
    }

    return success;
}

drmp3_int64 Mp3Decoder::do_tell() {
    if (!source_ || !source_->is_open()) {
        return 0;
    }
    size_t abs_pos = source_->tell();
    if (abs_pos < stream_base_offset_) {
        return 0;
    }
    return static_cast<drmp3_int64>(abs_pos - stream_base_offset_);
}

bool Mp3Decoder::reinit_decoder() {
    if (!mp3_) {
        return false;
    }

    drmp3_uninit(mp3_);
    memset(mp3_, 0, sizeof(drmp3));

    if (!drmp3_init(mp3_, on_read_cb, current_seek_cb(), current_tell_cb(), NULL, this, NULL)) {
        LOG_ERROR("Failed to reinitialize dr_mp3");
        initialized_ = false;
        return false;
    }

    initialized_ = true;
    return true;
}

drmp3_seek_proc Mp3Decoder::current_seek_cb() const {
    return (source_ && source_->is_seekable()) ? on_seek_cb : nullptr;
}

drmp3_tell_proc Mp3Decoder::current_tell_cb() const {
    return (source_ && source_->is_seekable()) ? on_tell_cb : nullptr;
}
