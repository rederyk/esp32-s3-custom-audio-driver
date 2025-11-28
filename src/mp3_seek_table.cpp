// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#include "mp3_seek_table.h"
#include "logger.h"
#include "data_source.h"
#include "dr_mp3.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <cstring>

Mp3SeekTable::~Mp3SeekTable() {
    clear();
}

void Mp3SeekTable::clear() {
    if (entries_) {
        heap_caps_free(entries_);
        entries_ = nullptr;
    }
    entry_count_ = 0;
    entry_capacity_ = 0;
    frames_per_entry_ = 0;
    current_pcm_frame_ = 0;
    last_entry_frame_ = 0;
    total_processed_bytes_ = 0;
    bytes_to_skip_ = 0;
    residue_len_ = 0;
}

bool Mp3SeekTable::ensure_capacity(size_t new_capacity) {
    if (new_capacity <= entry_capacity_) {
        return true;
    }

    size_t bytes_needed = new_capacity * sizeof(Entry);
    Entry* new_entries = static_cast<Entry*>(
        heap_caps_realloc(entries_, bytes_needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );

    if (!new_entries) {
        LOG_ERROR("Failed to allocate seek table: %u bytes", (unsigned)bytes_needed);
        return false;
    }

    entries_ = new_entries;
    entry_capacity_ = new_capacity;
    return true;
}

bool Mp3SeekTable::parse_header(const uint8_t* header, uint32_t* frame_size, uint32_t* samples_per_frame) {
    uint8_t b1 = header[1];
    uint8_t b2 = header[2];
    // uint8_t b3 = header[3]; // Not used

    int version_id = (b1 >> 3) & 0x03;      // MPEG version
    int layer_idx = (b1 >> 1) & 0x03;       // Layer
    int bitrate_idx = (b2 >> 4) & 0x0F;     // Bitrate index
    int sr_idx = (b2 >> 2) & 0x03;          // Sample rate index
    int padding = (b2 >> 1) & 0x01;         // Padding bit

    // Valida header
    if (layer_idx == 0 || bitrate_idx == 0x0F || sr_idx == 0x03) {
        return false;
    }

    // Tabelle bitrate (kbps) e sample rate
    static const uint16_t bitrate_table[2][16] = {
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},  // MPEG1
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}       // MPEG2/2.5
    };
    static const uint32_t sr_table[3][3] = {
        {44100, 48000, 32000},  // MPEG1
        {22050, 24000, 16000},  // MPEG2
        {11025, 12000, 8000}    // MPEG2.5
    };

    int version_row = (version_id == 0x03) ? 0 : 1;  // MPEG1 vs MPEG2/2.5
    uint32_t bitrate_kbps = bitrate_table[version_row][bitrate_idx];
    uint32_t sample_rate_hz = sr_table[(version_id == 0x03) ? 0 : (version_id == 0x02) ? 1 : 2][sr_idx];

    if (bitrate_kbps == 0 || sample_rate_hz == 0) {
        return false;
    }

    // Calcola frame size: FrameSize = 144 * BitRate / SampleRate + Padding
    // Note: For Layer 1 it is different but we focus on L3 mostly. L2 similar.
    // Simplified for Layer 2 & 3:
    *frame_size = (144 * bitrate_kbps * 1000) / sample_rate_hz + padding;

    // Calcola samples per frame
    *samples_per_frame = (layer_idx == 0x01) ? 1152 : 576;  // Layer 3 = 1152, others 576/384. Simplified.
    if (layer_idx == 3) *samples_per_frame = 384; // Layer 1
    else if (layer_idx == 2) *samples_per_frame = 1152; // Layer 2
    else *samples_per_frame = ((version_id == 0x03) ? 1152 : 576); // Layer 3 (MPEG1 vs 2/2.5)

    return true;
}

void Mp3SeekTable::begin(uint32_t sample_rate, uint32_t frames_per_entry) {
    clear();
    sample_rate_ = sample_rate;
    frames_per_entry_ = frames_per_entry;
    frames_per_entry_ = (frames_per_entry_ > 0) ? frames_per_entry_ : 4800; // safety default

    // Alloc initial capacity
    ensure_capacity(500);
}

bool Mp3SeekTable::append_chunk(const uint8_t* data, size_t size) {
    if (!data || size == 0) return true;

    size_t pos = 0;

    // 1. Gestione residuo precedente + inizio nuovo chunk
    if (residue_len_ > 0) {
        // Ho dei byte avanzati che non bastavano per un header (4 byte)
        // Provo a completare l'header con l'inizio del nuovo chunk
        size_t needed = 4 - residue_len_;
        if (size >= needed) {
            memcpy(residue_buf_ + residue_len_, data, needed);
            // Ora in residue_buf_ ho 4 byte. Provo a vedere se è un header valido
            if (residue_buf_[0] == 0xFF && (residue_buf_[1] & 0xE0) == 0xE0) {
                uint32_t frame_size = 0;
                uint32_t samples = 0;
                if (parse_header(residue_buf_, &frame_size, &samples)) {
                    // Valid header found spanning across chunks!
                    
                    // Quanto di questo frame è già nel residue (4 bytes)
                    // E quanto ne manca
                    if (frame_size < 4) { 
                        // Should not happen implementation wise as valid frames are > 20 bytes
                         residue_len_ = 0;
                         pos = 0; // re-scan from start? No, parsing failed effectively logic wise
                    } else {
                        bytes_to_skip_ = frame_size - 4; // skipped header already
                        
                        // Add entry if needed
                        if (current_pcm_frame_ - last_entry_frame_ >= frames_per_entry_) {
                             if (entry_count_ >= entry_capacity_) {
                                if (!ensure_capacity(entry_capacity_ + 500)) return false;
                             }
                             // The offset is where the frame STARTED.
                             // Frame start was: total_processed_bytes_ - residue_len_
                             entries_[entry_count_] = {current_pcm_frame_, total_processed_bytes_ - residue_len_};
                             entry_count_++;
                             last_entry_frame_ = current_pcm_frame_;
                        }
                        
                        current_pcm_frame_ += samples;
                        residue_len_ = 0; // consumed
                        pos = needed; // advanced into data
                    }
                } else {
                     // Invalid header in residue reconstruction.
                     // This is tricky. Just drop residue and start scan from data?
                     // Or maybe the sync was actually inside 'data'.
                     // For simplicity, drop residue content and scan 'data' from 0.
                     residue_len_ = 0;
                     pos = 0;
                }
            } else {
                 // Not a sync word. Drop and scan.
                 residue_len_ = 0;
                 pos = 0;
            }
        } else {
            // Not enough data even to complete header. Append all to residue and return.
            memcpy(residue_buf_ + residue_len_, data, size);
            residue_len_ += size;
            total_processed_bytes_ += size;
            return true;
        }
    }

    // 2. Skip frame body if we are in the middle of a frame
    if (bytes_to_skip_ > 0) {
        if (size - pos >= bytes_to_skip_) {
            pos += bytes_to_skip_;
            bytes_to_skip_ = 0;
        } else {
            bytes_to_skip_ -= (size - pos);
            total_processed_bytes_ += size;
            return true;
        }
    }

    // 3. Scan for headers in remaining data
    while (pos < size) {
        // Need at least 4 bytes for header
        if (pos + 4 > size) {
            // Save remaining to residue
            residue_len_ = size - pos;
            memcpy(residue_buf_, data + pos, residue_len_);
            break;
        }

        // Check sync
        if (data[pos] == 0xFF && (data[pos + 1] & 0xE0) == 0xE0) {
            uint32_t frame_size = 0;
            uint32_t samples = 0;
            if (parse_header(data + pos, &frame_size, &samples)) {
                // Add Entry
                 if (current_pcm_frame_ - last_entry_frame_ >= frames_per_entry_) {
                     if (entry_count_ >= entry_capacity_) {
                        if (!ensure_capacity(entry_capacity_ + 500)) return false;
                     }
                     entries_[entry_count_] = {current_pcm_frame_, total_processed_bytes_ + pos};
                     entry_count_++;
                     last_entry_frame_ = current_pcm_frame_;
                 }

                 current_pcm_frame_ += samples;
                 
                 // Advance
                 if (pos + frame_size <= size) {
                     pos += frame_size;
                 } else {
                     // Frame ends in next chunk
                     bytes_to_skip_ = frame_size - (size - pos);
                     pos = size; // Fully consumed
                 }
            } else {
                // False sync or invalid params
                pos++;
            }
        } else {
            pos++;
        }
    }

    total_processed_bytes_ += size;
    return true;
}

bool Mp3SeekTable::build(const uint8_t* mp3_data, size_t mp3_size, uint32_t sample_rate, uint32_t frames_per_entry) {
    // Use new incremental flow for one-shot build too, to avoid code duplication
    begin(sample_rate, frames_per_entry);
    
    uint32_t build_start = millis();
    LOG_INFO("Building seek table (entry every %u frames)...", frames_per_entry);

    bool res = append_chunk(mp3_data, mp3_size);

    uint32_t build_time = millis() - build_start;
    LOG_INFO("Seek table built: %u entries, %u bytes, %u ms (total frames: %llu)",
             (unsigned)entry_count_,
             (unsigned)memory_bytes(),
             build_time,
             current_pcm_frame_);
    
    return res && entry_count_ > 0;
}

bool Mp3SeekTable::find_seek_point(uint64_t target_frame, uint64_t* byte_offset, uint64_t* nearest_frame) const {
    if (!is_ready() || !byte_offset || !nearest_frame) {
        return false;
    }

    // Binary search upper_bound-ish
    // We want the entry s.t. entry.pcm_frame <= target_frame
    
    size_t left = 0;
    size_t right = entry_count_;
    intptr_t best = -1;

    while (left < right) {
        size_t mid = left + (right - left) / 2;

        if (entries_[mid].pcm_frame <= target_frame) {
            best = mid;
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    if (best != -1) {
        *byte_offset = entries_[best].byte_offset;
        *nearest_frame = entries_[best].pcm_frame;
        return true;
    }
    
    // If target is before first entry (should be rare given entry 0 is usually frame 0)
    // fallback to start
    *byte_offset = 0;
    *nearest_frame = 0;
    return true;
}
