// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.

#pragma once

#include <memory>
#include "audio_decoder.h"
#include "data_source.h"

// Factory per creare decoder audio
// Supporta auto-rilevamento del formato da estensione o contenuto
class AudioDecoderFactory {
public:
    // Crea decoder automaticamente rilevando il formato dalla sorgente
    // 1. Prova da estensione URI (se disponibile)
    // 2. Prova da magic bytes (ID3, RIFF, fLaC, ecc.)
    // Returns: unique_ptr al decoder o nullptr se formato non riconosciuto
    static std::unique_ptr<IAudioDecoder> create_from_source(IDataSource* source);

    // Crea decoder per formato specifico
    static std::unique_ptr<IAudioDecoder> create(AudioFormat format);

private:
    // Rileva formato da estensione file (.mp3, .wav, ecc.)
    static AudioFormat detect_from_extension(const char* uri);

    // Rileva formato da magic bytes nel contenuto
    static AudioFormat detect_from_content(IDataSource* source);
};
