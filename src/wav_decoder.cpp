// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#include "wav_decoder.h"
#include "logger.h"
#include <cstring>
#include <algorithm>

namespace {
    // Helper per leggere little-endian
    uint16_t read_u16_le(const uint8_t* buf) {
        return buf[0] | (buf[1] << 8);
    }

    uint32_t read_u32_le(const uint8_t* buf) {
        return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
    }
}

WavDecoder::~WavDecoder() {
    shutdown();
}

bool WavDecoder::init(IDataSource* source, size_t frames_per_chunk, bool build_seek_table) {
    if (!source || !source->is_open()) {
        LOG_ERROR("WavDecoder: DataSource not available or not open");
        return false;
    }

    source_ = source;

    if (!parse_wav_header()) {
        LOG_ERROR("WavDecoder: Failed to parse WAV header");
        return false;
    }

    // Validazione formato
    if (bits_per_sample_ != 16) {
        LOG_ERROR("WavDecoder: Only 16-bit PCM supported (got %u bits)", bits_per_sample_);
        return false;
    }

    if (channels_ != 1 && channels_ != 2) {
        LOG_ERROR("WavDecoder: Only mono/stereo supported (got %u channels)", channels_);
        return false;
    }

    initialized_ = true;
    current_frame_ = 0;

    LOG_INFO("WavDecoder initialized: %u Hz, %u ch, %u bits, %llu frames",
             sample_rate_, channels_, bits_per_sample_, total_frames_);

    return true;
}

void WavDecoder::shutdown() {
    source_ = nullptr;
    initialized_ = false;
    sample_rate_ = 0;
    channels_ = 0;
    bits_per_sample_ = 0;
    total_frames_ = 0;
    data_offset_ = 0;
    data_size_ = 0;
    current_frame_ = 0;
}

bool WavDecoder::parse_wav_header() {
    uint8_t header[44];

    // Leggi header WAV standard (minimo 44 bytes)
    source_->seek(0);
    size_t read = source_->read(header, sizeof(header));
    if (read < 44) {
        LOG_ERROR("WavDecoder: File too small for WAV header");
        return false;
    }

    // Verifica RIFF header
    if (memcmp(header, "RIFF", 4) != 0) {
        LOG_ERROR("WavDecoder: Missing RIFF signature");
        return false;
    }

    // Verifica WAVE format
    if (memcmp(header + 8, "WAVE", 4) != 0) {
        LOG_ERROR("WavDecoder: Missing WAVE signature");
        return false;
    }

    // Cerca chunk "fmt " e "data"
    size_t offset = 12;
    bool found_fmt = false;
    bool found_data = false;

    while (offset < source_->size() && (!found_fmt || !found_data)) {
        uint8_t chunk_header[8];
        source_->seek(offset);
        if (source_->read(chunk_header, 8) != 8) {
            break;
        }

        uint32_t chunk_size = read_u32_le(chunk_header + 4);

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            // Chunk fmt
            uint8_t fmt_data[16];
            if (source_->read(fmt_data, 16) < 16) {
                LOG_ERROR("WavDecoder: Invalid fmt chunk");
                return false;
            }

            uint16_t audio_format = read_u16_le(fmt_data);
            if (audio_format != 1) {
                LOG_ERROR("WavDecoder: Only PCM format supported (got format %u)", audio_format);
                return false;
            }

            channels_ = read_u16_le(fmt_data + 2);
            sample_rate_ = read_u32_le(fmt_data + 4);
            bits_per_sample_ = read_u16_le(fmt_data + 14);

            found_fmt = true;
            offset += 8 + chunk_size;

        } else if (memcmp(chunk_header, "data", 4) == 0) {
            // Chunk data
            data_offset_ = offset + 8;
            data_size_ = chunk_size;
            total_frames_ = data_size_ / (channels_ * (bits_per_sample_ / 8));
            found_data = true;
            break;

        } else {
            // Skip unknown chunk
            offset += 8 + chunk_size;
        }
    }

    if (!found_fmt || !found_data) {
        LOG_ERROR("WavDecoder: Missing fmt or data chunk");
        return false;
    }

    // Posiziona all'inizio dei dati
    source_->seek(data_offset_);

    return true;
}

uint64_t WavDecoder::read_frames(int16_t* dst, uint64_t frames) {
    if (!initialized_ || !source_) {
        return 0;
    }

    // Calcola quanti frame possiamo effettivamente leggere
    uint64_t frames_left = total_frames_ - current_frame_;
    uint64_t frames_to_read = std::min(frames, frames_left);

    if (frames_to_read == 0) {
        return 0; // EOF
    }

    size_t bytes_to_read = frames_to_read * channels_ * sizeof(int16_t);
    size_t bytes_read = source_->read(reinterpret_cast<uint8_t*>(dst), bytes_to_read);

    uint64_t frames_read = bytes_read / (channels_ * sizeof(int16_t));
    current_frame_ += frames_read;

    return frames_read;
}

bool WavDecoder::seek_to_frame(uint64_t frame_index) {
    if (!initialized_ || !source_) {
        return false;
    }

    if (!source_->is_seekable()) {
        LOG_WARN("WavDecoder: DataSource not seekable");
        return false;
    }

    if (frame_index >= total_frames_) {
        LOG_WARN("WavDecoder: Seek beyond EOF (requested %llu, total %llu)",
                 frame_index, total_frames_);
        return false;
    }

    size_t byte_offset = data_offset_ + (frame_index * channels_ * sizeof(int16_t));

    if (source_->seek(byte_offset)) {
        current_frame_ = frame_index;
        LOG_DEBUG("WavDecoder: Seeked to frame %llu (byte offset %u)", frame_index, (unsigned)byte_offset);
        return true;
    }

    LOG_ERROR("WavDecoder: Seek failed");
    return false;
}

uint32_t WavDecoder::bitrate() const {
    if (!initialized_ || sample_rate_ == 0) {
        return 0;
    }

    // PCM bitrate = sample_rate * channels * bits_per_sample / 1000 (kbps)
    return (sample_rate_ * channels_ * bits_per_sample_) / 1000;
}
