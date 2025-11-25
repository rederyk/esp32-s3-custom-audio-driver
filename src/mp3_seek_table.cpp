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

bool Mp3SeekTable::build(const uint8_t* mp3_data, size_t mp3_size, uint32_t sample_rate, uint32_t frames_per_entry) {
    clear();

    if (!mp3_data || mp3_size == 0) {
        LOG_ERROR("Invalid MP3 data for seek table");
        return false;
    }

    frames_per_entry_ = frames_per_entry;

    uint32_t build_start = millis();
    LOG_INFO("Building seek table (entry every %u frames = %.1f ms)...",
             frames_per_entry, (frames_per_entry * 1000.0f) / sample_rate);

    // Parser MP3 manualmente per trovare frame headers
    // Formato: 0xFF 0xE* (11 bit sync)
    size_t pos = 0;
    uint64_t pcm_frame_count = 0;
    uint64_t last_entry_frame = 0;
    size_t entries_created = 0;

    // Pre-alloca spazio stimato (assumendo ~25 frame/KB @ 128kbps)
    size_t estimated_entries = (mp3_size / 1024) / (frames_per_entry / 25) + 100;
    if (!ensure_capacity(estimated_entries)) {
        return false;
    }

    while (pos < mp3_size - 4) {
        // Cerca sync word MP3: 0xFF 0xEx
        if (mp3_data[pos] != 0xFF || (mp3_data[pos + 1] & 0xE0) != 0xE0) {
            pos++;
            continue;
        }

        // Trova! Parsa header MP3
        uint8_t b1 = mp3_data[pos + 1];
        uint8_t b2 = mp3_data[pos + 2];
        uint8_t b3 = mp3_data[pos + 3];

        int version_id = (b1 >> 3) & 0x03;      // MPEG version
        int layer_idx = (b1 >> 1) & 0x03;       // Layer
        int bitrate_idx = (b2 >> 4) & 0x0F;     // Bitrate index
        int sr_idx = (b2 >> 2) & 0x03;          // Sample rate index
        int padding = (b2 >> 1) & 0x01;         // Padding bit

        // Valida header
        if (layer_idx == 0 || bitrate_idx == 0x0F || sr_idx == 0x03) {
            pos++;
            continue;
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
            pos++;
            continue;
        }

        // Calcola frame size: FrameSize = 144 * BitRate / SampleRate + Padding
        uint32_t frame_size = (144 * bitrate_kbps * 1000) / sample_rate_hz + padding;

        if (frame_size < 24 || frame_size > 2881 || pos + frame_size > mp3_size) {
            pos++;
            continue;
        }

        // Verifica sync del prossimo frame (se possibile)
        if (pos + frame_size < mp3_size - 1) {
            if (mp3_data[pos + frame_size] != 0xFF || (mp3_data[pos + frame_size + 1] & 0xE0) != 0xE0) {
                pos++;
                continue;
            }
        }

        // Frame valido! Calcola samples per frame (sempre 1152 per Layer 3)
        uint32_t samples_per_frame = (layer_idx == 0x01) ? 1152 : 576;  // Layer 3 = 1152

        // Registra entry se è tempo
        if (pcm_frame_count - last_entry_frame >= frames_per_entry) {
            if (entry_count_ >= entry_capacity_) {
                if (!ensure_capacity(entry_capacity_ + 1000)) {
                    LOG_ERROR("Seek table full at %u entries", (unsigned)entry_count_);
                    break;
                }
            }

            entries_[entry_count_].pcm_frame = pcm_frame_count;
            entries_[entry_count_].byte_offset = pos;
            entry_count_++;
            last_entry_frame = pcm_frame_count;
            entries_created++;
        }

        pcm_frame_count += samples_per_frame;
        pos += frame_size;
    }

    // Aggiungi entry finale
    if (entry_count_ < entry_capacity_ && pcm_frame_count > last_entry_frame) {
        entries_[entry_count_].pcm_frame = pcm_frame_count;
        entries_[entry_count_].byte_offset = pos;
        entry_count_++;
    }

    uint32_t build_time = millis() - build_start;

    LOG_INFO("Seek table built: %u entries, %u bytes, %u ms (total frames: %llu)",
             (unsigned)entry_count_,
             (unsigned)memory_bytes(),
             build_time,
             pcm_frame_count);

    return entry_count_ > 0;
}

bool Mp3SeekTable::find_seek_point(uint64_t target_frame, uint64_t* byte_offset, uint64_t* nearest_frame) const {
    if (!is_ready() || !byte_offset || !nearest_frame) {
        return false;
    }

    // Binary search per entry più vicina <= target
    size_t left = 0;
    size_t right = entry_count_;
    size_t best = 0;

    while (left < right) {
        size_t mid = left + (right - left) / 2;

        if (entries_[mid].pcm_frame <= target_frame) {
            best = mid;
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    *byte_offset = entries_[best].byte_offset;
    *nearest_frame = entries_[best].pcm_frame;

    return true;
}
