# 🎯 Piano di Refactoring - Architettura DataSource Diretta

## Obiettivo
Implementare seeking nativo, supporto SD Card, HTTP streaming e salvataggio stato mediante un'architettura semplificata dove il decoder legge direttamente dalla sorgente dati.

## 🏗️ Nuova Architettura

```
┌─────────────────────┐
│   DataSource        │  ← Astrazione: File/SD/HTTP
│  - read()           │
│  - seek()           │
│  - tell()           │
└──────────┬──────────┘
           │ direct access (no buffer, no task)
           ↓
┌─────────────────────┐
│   Mp3Decoder        │  ← dr_mp3 con on_seek callback
│  - decode()         │
└──────────┬──────────┘
           │ PCM data (int16_t stereo)
           ↓
┌─────────────────────┐
│  Ring Buffer PCM    │  ← Solo per smoothing I2S (32-64KB)
│                     │
└──────────┬──────────┘
           │
           ↓
┌─────────────────────┐
│   Audio Task        │  ← Decode + I2S write (task singolo)
└─────────────────────┘
```

## 📦 Confronto Architettura

| Aspetto | **Attuale** | **Nuova** | Miglioramento |
|---------|-------------|-----------|---------------|
| **Tasks** | 2 (file + audio) | 1 (audio) | -50% overhead |
| **Ring buffer** | 128KB MP3 compressed | 32KB PCM | -75% memoria |
| **Decoder read** | Via ring buffer callback | Direct DataSource | Più semplice |
| **Seeking latency** | ~500ms brute force | <5ms nativo | **100x più veloce** |
| **Seek in pausa** | ❌ Non funziona | ✅ Funziona | ✅ |
| **Complessità sync** | Alta (2 tasks) | Bassa (1 task) | ⬇️ Semplice |

## 🎯 Fasi di Implementazione

### Fase 1: Astrazione DataSource (Foundation) 🏗️

**File da creare:**
- `src/data_source.h` - Interfaccia IDataSource
- `src/data_source_littlefs.h` - Implementazione LittleFS
- `src/data_source_sdcard.h` - Implementazione SD Card
- `src/data_source_http.h` - Implementazione HTTP Streaming

**Interfaccia IDataSource:**
```cpp
class IDataSource {
public:
    virtual ~IDataSource() = default;

    // Core I/O
    virtual size_t read(void* buffer, size_t size) = 0;
    virtual bool seek(size_t position) = 0;
    virtual size_t tell() const = 0;
    virtual size_t size() const = 0;

    // Lifecycle
    virtual bool open(const char* uri) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    // Capabilities
    virtual bool is_seekable() const = 0;
    virtual SourceType type() const = 0;
    virtual const char* uri() const = 0;
};
```

### Fase 2: Refactoring Mp3Decoder 🔄

**Modifiche a `mp3_decoder.h/cpp`:**
- Cambiare `init()` per accettare `IDataSource*` invece di `AudioPlayer*`
- Implementare `on_seek_cb` per dr_mp3 che usa `data_source->seek()`
- Rimuovere logica leftover buffer (non serve più)
- Aggiungere metodo `seek_to_frame()` pubblico

**Callback dr_mp3:**
```cpp
static size_t on_read_cb(void* user, void* buffer, size_t bytes) {
    Mp3Decoder* self = static_cast<Mp3Decoder*>(user);
    return self->source_->read(buffer, bytes);  // Direct read!
}

static drmp3_bool32 on_seek_cb(void* user, int offset, drmp3_seek_origin origin) {
    Mp3Decoder* self = static_cast<Mp3Decoder*>(user);
    size_t target = (origin == drmp3_seek_origin_start)
        ? offset
        : self->source_->tell() + offset;
    return self->source_->seek(target) ? DRMP3_TRUE : DRMP3_FALSE;
}
```

### Fase 3: Refactoring AudioPlayer 🎵

**Modifiche a `audio_player.h/cpp`:**
- **RIMUOVERE**: `file_stream_task`, `file_stream_task_entry`, `file_task_handle_`
- **RIMUOVERE**: Ring buffer MP3 compressed (`audio_ring_buffer_`)
- **AGGIUNGERE**: `std::unique_ptr<IDataSource> data_source_`
- **AGGIUNGERE**: Ring buffer PCM (32-64KB)
- **MODIFICARE**: `select_file()` → `select_source(const char* uri)`
- **MODIFICARE**: `arm_file()` → `arm_source()`
- **MODIFICARE**: `audio_task()` per:
  - Decoder legge direttamente da DataSource
  - Scrive PCM in ring buffer
  - Legge PCM da ring e scrive a I2S

**Nuova struttura audio_task:**
```cpp
void AudioPlayer::audio_task() {
    // 1. Init decoder con data_source
    decoder_.init(data_source_.get(), frames_per_chunk);

    // 2. Alloca ring buffer PCM (piccolo, 32KB)
    pcm_ring_buffer_ = xRingbufferCreate(...);

    // 3. Init codec & I2S
    codec_.init(...);
    i2s_driver_.init(...);

    // 4. Main loop
    while (!stop_requested_) {
        // Handle pause
        while (pause_flag_ && !stop_requested_) {
            vTaskDelay(20);
        }

        // Handle seek - ORA FUNZIONA IN PAUSA!
        if (seek_seconds_ >= 0) {
            uint64_t target_frame = seek_seconds_ * sample_rate;
            decoder_.seek_to_frame(target_frame);  // Seeking nativo!
            seek_seconds_ = -1;
        }

        // Decode: DataSource → PCM
        uint64_t frames = decoder_.read_frames(pcm_buffer, max_frames);
        if (frames == 0) break;  // End of stream

        // Write PCM → Ring buffer
        xRingbufferSend(pcm_ring_buffer_, pcm_buffer, pcm_bytes, timeout);

        // Read PCM from ring → I2S
        void* item = xRingbufferReceive(pcm_ring_buffer_, &size, 0);
        if (item) {
            i2s_write(I2S_NUM_0, item, size, &written, timeout);
            vRingbufferReturnItem(pcm_ring_buffer_, item);
        }
    }

    // 5. Cleanup
    decoder_.shutdown();
    vRingbufferDelete(pcm_ring_buffer_);
    i2s_driver_.uninstall();
}
```

### Fase 4: Supporto Multi-Source 🌐

**SD Card:**
- Setup SPI in `main.cpp`
- `SD.begin(CS_PIN)`
- URI format: `/sd/music/song.mp3`

**HTTP Streaming:**
- WiFi setup in `main.cpp`
- HTTPStreamSource con:
  - Buffer locale 4KB per chunking
  - Range requests support detection
  - Retry automatico su disconnessione
- URI format: `http://example.com/stream.mp3`

**Auto-detection in select_source:**
```cpp
bool AudioPlayer::select_source(const char* uri) {
    if (strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0) {
        data_source_ = std::make_unique<HTTPStreamSource>();
    } else if (strncmp(uri, "/sd/", 4) == 0) {
        data_source_ = std::make_unique<SDCardSource>();
    } else {
        data_source_ = std::make_unique<LittleFSSource>();
    }
    return data_source_->open(uri);
}
```

### Fase 5: State Persistence (Opzionale) 💾

**Salvataggio automatico via NVS:**
- Struct `PlaybackState` con URI, posizione, volume
- Auto-save ogni 5 secondi in `tick_housekeeping()`
- Resume al boot con prompt utente

## 📋 Checklist Implementazione

- [ ] Creare `data_source.h` con interfaccia
- [ ] Implementare `LittleFSSource`
- [ ] Implementare `SDCardSource`
- [ ] Implementare `HTTPStreamSource`
- [ ] Modificare `Mp3Decoder` per usare DataSource
- [ ] Aggiungere `on_seek_cb` a Mp3Decoder
- [ ] Rimuovere `file_stream_task` da AudioPlayer
- [ ] Convertire ring buffer MP3 → PCM
- [ ] Refactoring `audio_task`
- [ ] Modificare `select_file` → `select_source`
- [ ] Aggiornare comandi seriali in main.cpp
- [ ] Setup SD Card in main.cpp
- [ ] Setup WiFi in main.cpp
- [ ] Testing playback LittleFS
- [ ] Testing seeking nativo
- [ ] Testing seeking in pausa
- [ ] Testing playback SD Card
- [ ] Testing HTTP streaming
- [ ] (Opzionale) Implementare NVS state persistence

## 🎯 Metriche di Successo

| Metrica | Target |
|---------|--------|
| Seek latency (file locale) | **< 10ms** |
| Seek in pausa | ✅ **Funziona** |
| Supporto SD | ✅ |
| HTTP streaming | ✅ |
| Memoria ring buffer | **< 64KB** |
| Tasks attivi | **1** (vs 2 attuale) |
| LOC aggiunte | **< 1000** |

## 🚀 Vantaggi Finali

1. **Seeking 100x più veloce**: Nativo via dr_mp3, non brute force
2. **Funziona in pausa**: Decoder accede direttamente al file
3. **Più semplice**: 1 task invece di 2, no sincronizzazione complessa
4. **Meno memoria**: Ring buffer 4x più piccolo (PCM vs MP3)
5. **Estensibile**: Aggiungere nuove sorgenti richiede solo implementare IDataSource
6. **Multi-source unificato**: Stesso codice per File/SD/HTTP

---

**Data inizio refactoring:** 2025-11-25
**Stato:** 🚧 In corso
**Completamento stimato:** 6-9 giorni
