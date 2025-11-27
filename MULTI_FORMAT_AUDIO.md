# Multi-Format Audio Decoder - Guida Rapida

## 🎵 Formati Supportati

Il player ora supporta **4 formati audio** con auto-rilevamento automatico:

| Formato | Estensione | Decoder | Seek | Compressione |
|---------|------------|---------|------|--------------|
| **MP3** | `.mp3` | dr_mp3 | ✅ Seek table | Alta |
| **WAV** | `.wav` | Custom PCM | ✅ Istantaneo | Nessuna (PCM) |
| **AAC** | `.aac`, `.m4a` | Helix AAC | ❌ | Alta (+ SBR) |
| **FLAC** | `.flac` | libFLAC | ❌ | Lossless |

---

## 🎮 Comandi Serial per Test

Apri il monitor seriale (115200 baud) e usa questi comandi:

### Comandi di Playback
```
h  - Help (mostra tutti i comandi)
t  - Play MP3 test file (sample-rich.mp3)
s  - Play WAV sample (fileWAV1MG.wav)

1  - Test WAV 440Hz tone (sample_440hz.wav)
2  - Test AAC 440Hz tone (sample_440hz.aac)
3  - Test FLAC 440Hz tone (sample_440hz.flac)

p  - Play/Pause toggle
q  - Stop
i  - Info player (mostra formato corrente)
```

### Comandi Volume e Seek
```
v75  - Volume 75%
s5   - Seek indietro 5 secondi
```

---

## 📂 File di Test in `data/`

I seguenti file sono pronti per il test:

### File Audio Generati (440Hz tone, 3 secondi, stereo 44.1kHz)
- `sample_440hz.wav` - 517 KB (WAV PCM 16-bit)
- `sample_440hz.aac` - 49 KB (AAC ADTS)
- `sample_440hz.flac` - 45 KB (FLAC lossless)

### File Esistenti
- `sample-rich.mp3` - 302 KB (MP3)
- `fileWAV1MG.wav` - 1 MB (WAV)

---

## 🚀 Auto-Rilevamento Formato

Il sistema rileva automaticamente il formato in 2 modi:

### 1. Da Estensione File
```cpp
.mp3  → AudioFormat::MP3
.wav  → AudioFormat::WAV
.aac  → AudioFormat::AAC
.flac → AudioFormat::FLAC
.m4a  → AudioFormat::AAC
```

### 2. Da Magic Bytes (Header)
```
MP3:  ID3 tag o MPEG sync (0xFF 0xEx)
WAV:  RIFF....WAVE
AAC:  ADTS header (0xFF 0xFx)
FLAC: fLaC marker
```

---

## 🔧 Architettura

### Interfaccia Comune
```cpp
class IAudioDecoder {
    virtual bool init(IDataSource* source, ...);
    virtual uint64_t read_frames(int16_t* dst, uint64_t frames);
    virtual bool seek_to_frame(uint64_t frame_index);
    virtual uint32_t sample_rate() const;
    virtual uint32_t channels() const;
    virtual AudioFormat format() const;
};
```

### Factory Pattern
```cpp
// Auto-detection
auto decoder = AudioDecoderFactory::create_from_source(source);

// Explicit format
auto decoder = AudioDecoderFactory::create(AudioFormat::FLAC);
```

### Decoder Adapters
- `Mp3DecoderAdapter` - Wrapper per decoder esistente
- `WavDecoder` - Implementazione custom PCM
- `AacDecoderAdapter` - Helix AAC Decoder
- `FlacDecoderAdapter` - libFLAC

---

## 📊 Utilizzo Memoria

```
RAM:   14.4% (47312 bytes / 327680 bytes)
Flash: 17.8% (1166597 bytes / 6553600 bytes)
```

**Incremento rispetto a solo MP3:** ~84 KB Flash

---

## 🎯 Test Workflow

1. **Upload file audio** a LittleFS:
   ```bash
   pio run --target uploadfs
   ```

2. **Flash firmware**:
   ```bash
   pio run --target upload
   ```

3. **Apri monitor seriale** (115200 baud)

4. **Testa formati**:
   ```
   Press '1' → WAV playback
   Press 'i' → Check format detected
   Press '2' → AAC playback
   Press 'i' → Check format detected
   Press '3' → FLAC playback
   Press 'i' → Check format detected
   ```

5. **Verifica auto-detection**:
   ```
   AudioStream: Initialized WAV stream (44100 Hz, 2 ch)
   AudioStream: Initialized AAC stream (44100 Hz, 2 ch)
   AudioStream: Initialized FLAC stream (44100 Hz, 2 ch)
   ```

---

## ➕ Aggiungere Altri Formati

Per aggiungere un nuovo formato (es. OGG Vorbis):

### 1. Crea Adapter
```cpp
// ogg_decoder_adapter.h
class OggDecoderAdapter : public IAudioDecoder {
    // Implementa metodi virtuali
};
```

### 2. Aggiungi a Factory
```cpp
// audio_decoder_factory.cpp
case AudioFormat::OGG:
    return std::unique_ptr<IAudioDecoder>(new OggDecoderAdapter());
```

### 3. Aggiungi Detection
```cpp
// Estensione
if (strcmp(ext_lower, "ogg") == 0) {
    return AudioFormat::OGG;
}

// Magic bytes
if (memcmp(magic, "OggS", 4) == 0) {
    return AudioFormat::OGG;
}
```

**Done!** 🎉

---

## 📝 Note

- **AAC SBR**: Abilitato automaticamente su ESP32-S3 con PSRAM
- **FLAC Limits**: Max blocksize 8192, 16-bit samples
- **WAV**: Solo PCM 16-bit stereo/mono supportato
- **Seek**: Solo MP3 e WAV supportano seek, AAC/FLAC playback only

---

## 🐛 Troubleshooting

### File non rilevato
- Verifica estensione file corretta
- Controlla magic bytes del file
- Usa formato esplicito: `stream->begin(source, AudioFormat::AAC)`

### Decode error
- Verifica formato file (usa `file` command su Linux)
- Controlla sample rate supportato
- AAC: solo ADTS format
- FLAC: solo blocksize ≤ 8192

### Out of memory
- AAC con SBR richiede ~60KB DRAM aggiuntivi
- Usa PSRAM per buffering se disponibile
- Riduci `kFramesPerChunk` in audio_stream.cpp

---

**Implementato da:** Claude Code Assistant
**Data:** 27 Novembre 2025
**Versione:** 1.0
