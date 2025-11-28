// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include <cstdint>
#include <cstddef>

// Seek table per MP3: mappa frame PCM → byte offset nel file
// Permette seek istantanei (<10ms) invece di scansione lineare (secondi)
class Mp3SeekTable {
public:
    Mp3SeekTable() = default;
    ~Mp3SeekTable();

    // Costruisce la seek table scansionando il file (One-shot)
    bool build(const uint8_t* mp3_data, size_t mp3_size, uint32_t sample_rate, uint32_t frames_per_entry = 4800);

    // --- Incremental Build API ---
    // Inizializza il processo di costruzione incrementale
    void begin(uint32_t sample_rate, uint32_t frames_per_entry = 4800);

    // Processa un chunk di dati MP3. Assumiamo che i chunk siano contigui.
    // Ritorna true se ok, false se errore critico (allocazione memoria)
    bool append_chunk(const uint8_t* data, size_t size);

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

    // Stato build incrementale
    uint32_t sample_rate_ = 44100;
    uint64_t current_pcm_frame_ = 0;
    uint64_t last_entry_frame_ = 0;
    uint64_t total_processed_bytes_ = 0;
    
    // Gestione frame a cavallo di chunk
    size_t bytes_to_skip_ = 0;         // Byte del frame corrente che continuano nel prossimo chunk
    
    // Residuo per header sync (max 4 byte per parsare l'header se spezzato)
    uint8_t residue_buf_[4];
    size_t residue_len_ = 0;

    bool ensure_capacity(size_t new_capacity);
    bool parse_header(const uint8_t* header, uint32_t* frame_size, uint32_t* samples_per_frame);
};
