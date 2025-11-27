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

    // 1. Prova rilevamento da estensione
    const char* uri = source->uri();
    if (uri) {
        format = detect_from_extension(uri);
        if (format != AudioFormat::UNKNOWN) {
            LOG_INFO("AudioDecoderFactory: Detected format %s from extension",
                     audio_format_to_string(format));
        }
    }

    // 2. Se estensione non disponibile o sconosciuta, prova magic bytes
    if (format == AudioFormat::UNKNOWN) {
        format = detect_from_content(source);
        if (format != AudioFormat::UNKNOWN) {
            LOG_INFO("AudioDecoderFactory: Detected format %s from content",
                     audio_format_to_string(format));
        }
    }

    // 3. Crea decoder appropriato
    if (format == AudioFormat::UNKNOWN) {
        LOG_ERROR("AudioDecoderFactory: Unable to detect audio format");
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

    // Leggi primi bytes per magic number
    uint8_t magic[12];
    source->seek(0);
    size_t read = source->read(magic, sizeof(magic));

    // Ripristina posizione originale
    source->seek(original_pos);

    if (read < 4) {
        return AudioFormat::UNKNOWN;
    }

    // Controlla magic bytes
    // MP3: ID3v2 header o MPEG frame sync
    if (read >= 3 && memcmp(magic, "ID3", 3) == 0) {
        return AudioFormat::MP3;
    }
    if (read >= 2 && magic[0] == 0xFF && (magic[1] & 0xE0) == 0xE0) {
        // MPEG frame sync (11 bit a 1)
        return AudioFormat::MP3;
    }

    // WAV: RIFF header
    if (read >= 12 && memcmp(magic, "RIFF", 4) == 0 && memcmp(magic + 8, "WAVE", 4) == 0) {
        return AudioFormat::WAV;
    }

    // FLAC: fLaC marker
    if (read >= 4 && memcmp(magic, "fLaC", 4) == 0) {
        return AudioFormat::FLAC;
    }

    // AAC: ADTS header (0xFF 0xFx)
    if (read >= 2 && magic[0] == 0xFF && (magic[1] & 0xF0) == 0xF0) {
        return AudioFormat::AAC;
    }

    return AudioFormat::UNKNOWN;
}
