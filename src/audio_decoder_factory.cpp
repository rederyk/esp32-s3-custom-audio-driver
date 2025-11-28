// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.

#include "audio_decoder_factory.h"
#include "mp3_decoder_adapter.h"
#include "wav_decoder.h"
#include "logger.h"
#include <cstring>
#include <cctype>

namespace {
    // Helper per convertire stringa a lowercase
    void to_lower(char* str) {
        for (char* p = str; *p; ++p) {
            *p = tolower(*p);
        }
    }

    // Helper per estrarre estensione da URI
    const char* get_extension(const char* uri) {
        if (!uri) return nullptr;

        const char* dot = strrchr(uri, '.');
        if (!dot || dot == uri) return nullptr;

        return dot + 1; // Salta il '.'
    }
}

std::unique_ptr<IAudioDecoder> AudioDecoderFactory::create_from_source(IDataSource* source) {
    if (!source) {
        LOG_ERROR("AudioDecoderFactory: null source");
        return nullptr;
    }

    AudioFormat format = AudioFormat::UNKNOWN;

    // Diagnostic buffer for logging
    uint8_t diagnostic_buffer[32];
    size_t diagnostic_read = 0;
    size_t source_size = 0;

    // Capture source details
    const char* uri = source->uri();
    source_size = source->size();

    // 1. Try detection from extension
    if (uri) {
        format = detect_from_extension(uri);
        if (format != AudioFormat::UNKNOWN) {
            LOG_INFO("AudioDecoderFactory: Detected format %s from extension",
                     audio_format_to_string(format));
        }
    }

    // 2. If extension detection fails, try magic bytes
    if (format == AudioFormat::UNKNOWN) {
        // Capture diagnostic information
        size_t original_pos = source->tell();
        source->seek(0);
        diagnostic_read = source->read(diagnostic_buffer, sizeof(diagnostic_buffer));
        source->seek(original_pos);

        format = detect_from_content(source);
        if (format != AudioFormat::UNKNOWN) {
            LOG_INFO("AudioDecoderFactory: Detected format %s from content", 
                     audio_format_to_string(format));
        } else {
            // Enhanced diagnostic logging for unrecognized streams
            LOG_ERROR("AudioDecoderFactory: Format detection FAILED");
            LOG_DEBUG("Stream Diagnostic Information:");
            LOG_DEBUG(" URI: %s", uri ? uri : "UNKNOWN");
            LOG_DEBUG(" Total Stream Size: %u bytes", (unsigned)source_size);
            LOG_DEBUG(" First %u bytes:", (unsigned)diagnostic_read);
            
            // Hex dump of first bytes
            char hex_dump[128] = {0};
            char* hex_ptr = hex_dump;
            for (size_t i = 0; i < diagnostic_read; ++i) {
                hex_ptr += sprintf(hex_ptr, "%02X ", diagnostic_buffer[i]);
            }
            LOG_DEBUG(" Hex: %s", hex_dump);

            // Printable ASCII representation
            char ascii_dump[33] = {0};
            for (size_t i = 0; i < diagnostic_read; ++i) {
                ascii_dump[i] = (diagnostic_buffer[i] >= 32 && diagnostic_buffer[i] <= 126) 
                    ? diagnostic_buffer[i] : '.';
            }
            LOG_DEBUG(" ASCII: %s", ascii_dump);
        }
    }

    // 3. Create appropriate decoder
    if (format == AudioFormat::UNKNOWN) {
        LOG_ERROR("AudioDecoderFactory: Definitive failure - Unable to detect audio format");
        return nullptr;
    }

    return create(format);
}

std::unique_ptr<IAudioDecoder> AudioDecoderFactory::create(AudioFormat format) {
    switch (format) {
        case AudioFormat::MP3:
            LOG_DEBUG("AudioDecoderFactory: Creating Mp3Decoder");
            return std::unique_ptr<IAudioDecoder>(new Mp3DecoderAdapter());

        case AudioFormat::WAV:
            LOG_DEBUG("AudioDecoderFactory: Creating WavDecoder");
            return std::unique_ptr<IAudioDecoder>(new WavDecoder());

        case AudioFormat::AAC:
            LOG_WARN("AudioDecoderFactory: AAC not yet implemented");
            return nullptr;

        case AudioFormat::FLAC:
            LOG_WARN("AudioDecoderFactory: FLAC not yet implemented");
            return nullptr;

        default:
            LOG_ERROR("AudioDecoderFactory: Unknown format");
            return nullptr;
    }
}

AudioFormat AudioDecoderFactory::detect_from_extension(const char* uri) {
    const char* ext = get_extension(uri);
    if (!ext) {
        return AudioFormat::UNKNOWN;
    }

    // Copia estensione e converti a lowercase
    char ext_lower[16];
    strncpy(ext_lower, ext, sizeof(ext_lower) - 1);
    ext_lower[sizeof(ext_lower) - 1] = '\0';
    to_lower(ext_lower);

    // Match estensioni comuni
    if (strcmp(ext_lower, "mp3") == 0) {
        return AudioFormat::MP3;
    } else if (strcmp(ext_lower, "wav") == 0) {
        return AudioFormat::WAV;
    } else if (strcmp(ext_lower, "aac") == 0 || strcmp(ext_lower, "m4a") == 0) {
        return AudioFormat::AAC;
    } else if (strcmp(ext_lower, "flac") == 0) {
        return AudioFormat::FLAC;
    }

    return AudioFormat::UNKNOWN;
}

AudioFormat AudioDecoderFactory::detect_from_content(IDataSource* source) {
    if (!source || !source->is_open()) {
        return AudioFormat::UNKNOWN;
    }

    // Salva posizione corrente
    size_t original_pos = source->tell();

    // Leggi primi bytes per magic number - increased buffer to scan for sync patterns
    uint8_t magic[1024];  // Larger buffer to handle metadata and find sync
    source->seek(0);
    size_t read = source->read(magic, sizeof(magic));

    // Ripristina posizione originale
    source->seek(original_pos);

    if (read < 4) {
        return AudioFormat::UNKNOWN;
    }

    // Enhanced MP3 Detection
    // 1. ID3v2 Header
    if (read >= 3 && memcmp(magic, "ID3", 3) == 0) {
        return AudioFormat::MP3;
    }

    // 2. Scan for MPEG Frame Sync (skip potential Icecast metadata)
    for (size_t i = 0; i < read - 1; ++i) {
        if (magic[i] == 0xFF && (magic[i+1] & 0xE0) == 0xE0) {
            // Found potential MPEG sync, validate
            if (i + 3 < read) {
                uint8_t version = (magic[i+1] >> 3) & 0x03;
                uint8_t layer = (magic[i+1] >> 1) & 0x03;

                // Check for valid MPEG Audio Layer III (MP3)
                if (version != 0x01 && layer == 0x01) {
                    return AudioFormat::MP3;
                }
            }
        }
    }

    // 3. Scan for AAC ADTS sync
    for (size_t i = 0; i < read - 1; ++i) {
        if (magic[i] == 0xFF && (magic[i+1] & 0xF0) == 0xF0) {
            return AudioFormat::AAC;
        }
    }

    // WAV: RIFF header
    if (read >= 12 && memcmp(magic, "RIFF", 4) == 0 && memcmp(magic + 8, "WAVE", 4) == 0) {
        return AudioFormat::WAV;
    }

    // FLAC: fLaC marker
    if (read >= 4 && memcmp(magic, "fLaC", 4) == 0) {
        return AudioFormat::FLAC;
    }

    return AudioFormat::UNKNOWN;
}
