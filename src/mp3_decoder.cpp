#define DR_MP3_IMPLEMENTATION
#include "mp3_decoder.h"

#include <cstring>
#include <esp_heap_caps.h>
#include "audio_player.h"
#include "logger.h"

namespace {
constexpr uint32_t kBytesPerSample = sizeof(int16_t);
constexpr uint32_t kDefaultChannels = 2;
}

Mp3Decoder::~Mp3Decoder() {
    shutdown();
}

bool Mp3Decoder::ensure_buffers(size_t pcm_frames, size_t leftover_bytes) {
    size_t pcm_bytes = pcm_frames * kDefaultChannels * kBytesPerSample;
    bool pcm_ok = true;
    bool leftover_ok = true;

    if (buffers_.pcm == nullptr || buffers_.pcm_capacity_frames < pcm_frames) {
        int16_t *new_pcm = static_cast<int16_t *>(heap_caps_realloc(buffers_.pcm, pcm_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!new_pcm) {
            pcm_ok = false;
        } else {
            buffers_.pcm = new_pcm;
            buffers_.pcm_capacity_frames = pcm_frames;
        }
    }

    if (buffers_.leftover == nullptr || buffers_.leftover_capacity < leftover_bytes) {
        uint8_t *new_leftover = static_cast<uint8_t *>(heap_caps_realloc(buffers_.leftover, leftover_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!new_leftover) {
            leftover_ok = false;
        } else {
            buffers_.leftover = new_leftover;
            buffers_.leftover_capacity = leftover_bytes;
        }
    }

    return pcm_ok && leftover_ok;
}

bool Mp3Decoder::init(AudioPlayer *owner, size_t frames_per_chunk, size_t leftover_capacity) {
    owner_ = owner;
    leftover_size_ = 0;
    leftover_offset_ = 0;

    if (!ensure_buffers(frames_per_chunk, leftover_capacity)) {
        LOG_ERROR("Impossibile allocare il buffer pool (pcm %u frames, leftover %u bytes)",
                  static_cast<unsigned>(frames_per_chunk),
                  static_cast<unsigned>(leftover_capacity));
        return false;
    }

    mp3_ = static_cast<drmp3 *>(heap_caps_calloc(1, sizeof(drmp3), MALLOC_CAP_SPIRAM));
    if (!mp3_) {
        LOG_ERROR("Impossibile allocare il decoder MP3 (heap SPIRAM esaurito?).");
        return false;
    }

    if (!drmp3_init(mp3_, on_read_cb, NULL, NULL, NULL, this, NULL)) {
        LOG_ERROR("Impossibile inizializzare il decodificatore MP3.");
        heap_caps_free(mp3_);
        mp3_ = nullptr;
        return false;
    }

    initialized_ = true;
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
    if (buffers_.leftover) {
        heap_caps_free(buffers_.leftover);
        buffers_.leftover = nullptr;
    }
    buffers_.pcm_capacity_frames = 0;
    buffers_.leftover_capacity = 0;
    leftover_size_ = 0;
    leftover_offset_ = 0;
    initialized_ = false;
}

drmp3_uint64 Mp3Decoder::read_frames(int16_t *dst, drmp3_uint64 frames) {
    if (!mp3_ || !initialized_) {
        return 0;
    }
    return drmp3_read_pcm_frames_s16(mp3_, frames, dst);
}

drmp3_uint64 Mp3Decoder::total_frames() const {
    if (!mp3_ || !initialized_) {
        return 0;
    }
    return drmp3_get_pcm_frame_count(mp3_);
}

size_t Mp3Decoder::on_read_cb(void *user, void *buffer, size_t bytesToRead) {
    Mp3Decoder *self = static_cast<Mp3Decoder *>(user);
    return self ? self->fill_buffer(static_cast<uint8_t *>(buffer), bytesToRead) : 0;
}

size_t Mp3Decoder::fill_buffer(uint8_t *out, size_t bytes_to_read) {
    if (!owner_) {
        return 0;
    }
    size_t total_copied = 0;

    if (leftover_size_ > leftover_offset_) {
        size_t avail = leftover_size_ - leftover_offset_;
        size_t copy_now = (avail < bytes_to_read) ? avail : bytes_to_read;
        memcpy(out, buffers_.leftover + leftover_offset_, copy_now);
        leftover_offset_ += copy_now;
        total_copied += copy_now;
        if (leftover_offset_ >= leftover_size_) {
            leftover_size_ = 0;
            leftover_offset_ = 0;
        }
    }

    while (total_copied < bytes_to_read && !owner_->should_stop()) {
        void *item = nullptr;
        size_t item_size = 0;
        if (!owner_->receive_ring_item(&item, &item_size)) {
            break;
        }

        size_t space_left = bytes_to_read - total_copied;
        size_t copy_size = (item_size < space_left) ? item_size : space_left;
        memcpy(out + total_copied, item, copy_size);
        total_copied += copy_size;

        if (item_size > copy_size) {
            size_t remaining = item_size - copy_size;
            if (buffers_.leftover && remaining <= buffers_.leftover_capacity) {
                memcpy(buffers_.leftover, static_cast<uint8_t *>(item) + copy_size, remaining);
                leftover_size_ = remaining;
                leftover_offset_ = 0;
            } else {
                LOG_WARN("Chunk eccedente (%u bytes) supera buffer leftover (%u), scarto",
                         static_cast<unsigned>(remaining),
                         static_cast<unsigned>(buffers_.leftover_capacity));
            }
        }

        owner_->return_ring_item(item);
    }

    return total_copied;
}
