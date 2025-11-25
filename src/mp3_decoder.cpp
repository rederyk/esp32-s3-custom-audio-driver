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

    if (!ensure_buffers(frames_per_chunk)) {
        LOG_ERROR("Failed to allocate PCM buffer (%u frames)", static_cast<unsigned>(frames_per_chunk));
        return false;
    }

    mp3_ = static_cast<drmp3 *>(heap_caps_calloc(1, sizeof(drmp3), MALLOC_CAP_SPIRAM));
    if (!mp3_) {
        LOG_ERROR("Failed to allocate drmp3 struct");
        return false;
    }

    // Inizializza dr_mp3 con callbacks per read, seek e tell
    if (!drmp3_init(mp3_, on_read_cb, on_seek_cb, on_tell_cb, NULL, this, NULL)) {
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

    uint32_t current_pos = source_->tell();
    uint32_t seek_start = millis();

    // Usa seek table se disponibile
    if (seek_table_.is_ready()) {
        uint64_t byte_offset = 0;
        uint64_t nearest_frame = 0;

        if (seek_table_.find_seek_point(frame_index, &byte_offset, &nearest_frame)) {
            LOG_DEBUG("Seek table: target=%llu, nearest=%llu (delta=%lld frames), byte=%llu",
                      frame_index, nearest_frame,
                      (int64_t)(frame_index - nearest_frame),
                      byte_offset);

            // Seek diretto alla posizione byte
            if (source_->seek(byte_offset)) {
                // Reinizializza dr_mp3 dalla nuova posizione senza fare seek interno
                // Questo resetterà il suo buffer interno e ripartirà dalla posizione corrente del file
                drmp3_uninit(mp3_);
                if (!drmp3_init(mp3_, on_read_cb, on_seek_cb, on_tell_cb, NULL, this, NULL)) {
                    LOG_ERROR("Failed to reinit dr_mp3 after seek table jump");
                    return false;
                }

                // Ora il decoder è alla posizione nearest_frame nel file
                // Decode e scarta frame fino al target esatto
                uint64_t frames_to_skip = frame_index - nearest_frame;

                if (frames_to_skip > 0) {
                    int16_t temp_buf[2048];
                    uint64_t total_skipped = 0;

                    while (total_skipped < frames_to_skip) {
                        uint64_t skip_now = (frames_to_skip - total_skipped > 1024) ? 1024 : (frames_to_skip - total_skipped);
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
            } else {
                LOG_ERROR("Seek table byte seek failed, falling back to dr_mp3");
            }
        }
    }

    // Fallback a dr_mp3 seek (lento)
    LOG_DEBUG("Decoder seek_to_frame (dr_mp3 fallback): current file pos=%u, target frame=%llu",
              (unsigned)current_pos, frame_index);

    drmp3_bool32 result = drmp3_seek_to_pcm_frame(mp3_, frame_index);
    uint32_t seek_end = millis();

    uint32_t new_pos = source_->tell();

    if (result == DRMP3_TRUE) {
        LOG_INFO("dr_mp3 seek: %u ms, file pos %u -> %u (jumped %d bytes)",
                 seek_end - seek_start,
                 (unsigned)current_pos,
                 (unsigned)new_pos,
                 (int)(new_pos - current_pos));
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

    size_t current_pos = source_->tell();
    size_t target_pos = 0;

    switch (origin) {
        case DRMP3_SEEK_SET:
            target_pos = offset;
            break;

        case DRMP3_SEEK_CUR:
            target_pos = current_pos + offset;
            break;

        case DRMP3_SEEK_END:
            // Seek from end: offset è negativo dalla fine del file
            if (source_->size() == 0) {
                LOG_WARN("Cannot seek from end: file size unknown");
                return false;
            }
            target_pos = source_->size() + offset;  // offset è negativo
            break;

        default:
            LOG_WARN("Unsupported seek origin: %d", origin);
            return false;
    }

    // Esegui seek sulla DataSource
    uint32_t seek_start = millis();
    bool success = source_->seek(target_pos);
    uint32_t seek_end = millis();

    static int seek_call_count = 0;
    seek_call_count++;

    if (success) {
        LOG_DEBUG("do_seek #%d: origin=%d, %u -> %u (%+d bytes) in %u ms",
                  seek_call_count, (int)origin,
                  (unsigned)current_pos, (unsigned)target_pos,
                  (int)(target_pos - current_pos),
                  seek_end - seek_start);
    } else {
        LOG_ERROR("do_seek #%d FAILED: target byte %u", seek_call_count, (unsigned)target_pos);
    }

    return success;
}

drmp3_int64 Mp3Decoder::do_tell() {
    if (!source_ || !source_->is_open()) {
        return 0;
    }
    return static_cast<drmp3_int64>(source_->tell());
}
