#pragma once

#include <cstdint>
#include <cstddef>

// Seek table per MP3: mappa frame PCM → byte offset nel file
// Permette seek istantanei (<10ms) invece di scansione lineare (secondi)
class Mp3SeekTable {
public:
    Mp3SeekTable() = default;
    ~Mp3SeekTable();

    // Costruisce la seek table scansionando il file
    // frames_per_entry: quanti frame tra ogni entry (default 4800 = ~100ms @ 48kHz)
    // Ritorna: true se successo, false se errore
    bool build(const uint8_t* mp3_data, size_t mp3_size, uint32_t sample_rate, uint32_t frames_per_entry = 4800);

    // Trova il seek point più vicino al target frame
    // Ritorna: true se trovato, false se table vuota
    // Output: byte_offset = posizione byte nel file, nearest_frame = frame del seek point
    bool find_seek_point(uint64_t target_frame, uint64_t* byte_offset, uint64_t* nearest_frame) const;

    bool is_ready() const { return entries_ != nullptr && entry_count_ > 0; }
    size_t size() const { return entry_count_; }
    size_t memory_bytes() const { return entry_count_ * sizeof(Entry); }

    void clear();

private:
    struct Entry {
        uint64_t pcm_frame;      // Numero frame PCM
        uint64_t byte_offset;    // Offset byte nel file MP3
    };

    Entry* entries_ = nullptr;         // Array dinamico allocato con heap_caps_malloc
    size_t entry_count_ = 0;           // Numero di entry nella table
    size_t entry_capacity_ = 0;        // Capacità allocata
    uint32_t frames_per_entry_ = 0;    // Frame tra ogni entry

    bool ensure_capacity(size_t new_capacity);
};
