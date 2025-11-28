// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.

#pragma once

#include <cstddef>
#include <cstdint>

// Forward declaration
class IDataSource;

enum class AudioFormat {
    MP3,
    AAC,
    FLAC,
    WAV,
    UNKNOWN
};

// Interfaccia comune per tutti i decoder audio
class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    // Inizializzazione del decoder
    // source: sorgente dati (file, HTTP, ecc.)
    // frames_per_chunk: numero di frame PCM per chunk di decodifica
    // build_seek_table: se true, costruisce seek table per seek veloce (opzionale)
    virtual bool init(IDataSource* source, size_t frames_per_chunk, bool build_seek_table = true) = 0;

    // Shutdown e cleanup
    virtual void shutdown() = 0;

    // Decodifica frames PCM
    // dst: buffer di destinazione per PCM interleaved (L,R,L,R...)
    // frames: numero di frame da leggere
    // Returns: numero effettivo di frame letti (può essere < frames se EOF)
    virtual uint64_t read_frames(int16_t* dst, uint64_t frames) = 0;

    // Seek a frame specifico
    // frame_index: indice del frame PCM target
    // Returns: true se seek riuscito
    virtual bool seek_to_frame(uint64_t frame_index) = 0;

    // Informazioni sul flusso audio
    virtual uint32_t sample_rate() const = 0;
    virtual uint32_t channels() const = 0;
    virtual uint64_t total_frames() const = 0;
    virtual bool initialized() const = 0;
    virtual AudioFormat format() const = 0;

    // Bitrate in kbps (opzionale, ritorna 0 se non calcolabile)
    virtual uint32_t bitrate() const { return 0; }

    // Seek table support (opzionale, ritorna false se non supportato)
    virtual bool has_seek_table() const { return false; }
};

// Helper per convertire AudioFormat a stringa
inline const char* audio_format_to_string(AudioFormat format) {
    switch (format) {
        case AudioFormat::MP3: return "MP3";
        case AudioFormat::AAC: return "AAC";
        case AudioFormat::FLAC: return "FLAC";
        case AudioFormat::WAV: return "WAV";
        default: return "UNKNOWN";
    }
}
