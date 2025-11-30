# Architettura - openESPaudio

Panoramica dell'architettura interna della libreria.

## Overview Generale

openESPaudio è strutturato come libreria modulare per ESP32 con separazione chiara tra componenti hardware e logica di riproduzione.

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   AudioPlayer   │ -> │   AudioStream    │ -> │  AudioOutput    │
│   (Control)     │    │  (Decode/Stream) │    │   (Hardware)    │
└─────────────────┘    └──────────────────┘    └─────────────────┘
         │                       │                       │
         ▼                       ▼                       ▼
   User Interface        Data Sources            I2S Codec
   (Serial, API)         (File, HTTP)           (ES8311, etc.)
```

## Componenti Principali

### AudioPlayer

Classe di controllo principale che orchestra l'intera riproduzione.

**Responsabilità:**
- Gestione stati riproduzione (STOPPED, PLAYING, PAUSED, ENDED, ERROR)
- Interfaccia utente (play, pause, seek, volume)
- Coordinamento tra AudioStream e AudioOutput
- Callbacks eventi (on_start, on_stop, on_error, etc.)

**Pattern utilizzati:**
- State Machine per stati riproduzione
- Observer Pattern per callbacks
- RAII per gestione risorse

### AudioStream

Gestisce decodifica e streaming dei dati audio.

**Componenti interni:**
- **AudioDecoder**: Factory per decoder MP3/WAV
- **IDataSource**: Interfaccia astratta per sorgenti (file, HTTP, timeshift)
- **Ring Buffer**: Buffer circolare per smoothing I/O

**Responsabilità:**
- Lettura dati dalla sorgente
- Decodifica MP3/WAV in PCM
- Gestione buffer per evitare underrun
- Seek support (quando possibile)

### AudioOutput

Interfaccia con hardware audio (I2S codec).

**Responsabilità:**
- Configurazione I2S e codec
- Output PCM a sample rate corretto
- Controllo volume hardware/software
- Sincronizzazione clock

### TimeshiftManager

Implementa timeshift per streaming HTTP con buffer intelligente.

**Architettura Buffer:**
```
Download Task ──┐
                │
Recording Buffer├─ Chunk Writer ── PSRAM Pool ──┐
                │  (SD files)      (circular)    │
Playback Buffer ────────────────────────────────┘
                │
   AudioPlayer  │
```

**Caratteristiche chiave:**
- **Buffer adattivo**: Dimensioni calcolate dinamicamente da bitrate rilevato
- **Dual storage**: PSRAM (veloce, limitato) + SD card (lento, illimitato)
- **Chunk atomici**: Unità indivisibili da 128KB-512KB
- **Seek table**: Mappatura tempo→byte per seek preciso

## Data Sources

### Interfaccia IDataSource

```cpp
class IDataSource {
public:
    virtual size_t read(void* buffer, size_t size) = 0;
    virtual bool seek(size_t position) = 0;
    virtual size_t tell() const = 0;
    virtual size_t size() const = 0;
    virtual bool open(const char* uri) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    virtual bool is_seekable() const = 0;
    virtual SourceType type() const = 0;
};
```

### Implementazioni Concrete

- **DataSourceLittleFS**: File da flash interna
- **DataSourceSDCard**: File da SD card
- **DataSourceHTTP**: Stream HTTP diretto (no buffer)
- **TimeshiftManager**: Stream HTTP con buffer circolare

## Task Architecture

### FreeRTOS Tasks Utilizzati

1. **Audio Task** (AudioPlayer::audio_task)
   - Priorità: Media
   - Stack: 8KB
   - Core: Affinità configurabile
   - Responsabile: Riproduzione principale

2. **Download Task** (TimeshiftManager)
   - Priorità: Alta
   - Stack: 6KB
   - Core: 1 (WiFi)
   - Responsabile: Scaricamento HTTP

3. **Writer Task** (TimeshiftManager)
   - Priorità: Media
   - Stack: 4KB
   - Core: 0
   - Responsabile: Scrittura chunk su storage

4. **Preloader Task** (TimeshiftManager)
   - Priorità: Bassa
   - Stack: 3KB
   - Core: 0
   - Responsabile: Precaricamento chunk successivi

### Sincronizzazione

- **EventGroups**: Coordinamento tra task
- **Mutex**: Protezione buffer condivisi
- **Queues**: Comunicazione producer/consumer per chunk
- **Semaphores**: Gestione risorse critiche

## Memory Management

### Strategie Allocazione

- **PSRAM Pool**: Pre-allocato per timeshift PSRAM mode
- **Dynamic Allocation**: Per buffer variabili
- **Static Buffers**: Per strutture fisse
- **Ring Buffers**: Per smoothing I/O

### Memory Layout (ESP32-S3 con PSRAM)

```
SRAM (512KB):
├── Static data (~50KB)
├── Stack tasks (~32KB)
├── Heap generale (~350KB)
└── Ring buffer audio (~80KB)

PSRAM (2MB+):
├── PSRAM chunk pool (1.5MB)
├── Playback buffer (256KB)
└── Recording buffer (192KB)
```

## Decoder Architecture

### Factory Pattern

```cpp
class AudioDecoderFactory {
public:
    static std::unique_ptr<AudioDecoder> create(AudioFormat format);
};

class AudioDecoder {
public:
    virtual bool decode(const uint8_t* in, size_t in_size,
                       int16_t* out, size_t* out_samples) = 0;
    virtual void reset() = 0;
};
```

### Decoder Implementati

- **MP3Decoder**: Basato su dr_mp3 (senza seek table)
- **WAVDecoder**: PCM diretto
- **Extensible**: Facilmente aggiungibili nuovi formati

## Storage Subsystem

### SD Card Driver

**Caratteristiche:**
- Singleton pattern
- Auto-mount su primo accesso
- Error handling completo
- Thread-safe

**Filesystem Support:**
- FAT32 (principale)
- Supporto teoricamente estensibile ad altri

### Timeshift Storage Modes

#### PSRAM_ONLY Mode
- **Vantaggi**: Velocità massima, no I/O blocking
- **Limiti**: ~2 minuti buffer
- **Uso**: Contenuti brevi, interattivi

#### SD_CARD Mode
- **Vantaggi**: Buffer illimitato
- **Limiti**: Latenza seek più alta
- **Uso**: Sessioni lunghe, registrazione

### Runtime Storage Switching

**Implementazione:**
- **Chunk Migration**: Trasferimento progressivo tra backend
- **Seamless Playback**: Nessuna interruzione durante switch
- **Background Tasks**: Migrazione trasparente all'utente

## Error Handling

### Strategie

- **Graceful Degradation**: Fallback su modalità alternative
- **Recovery Automatico**: Retry con backoff esponenziale
- **Error Propagation**: Callbacks per gestione errori applicativa
- **Logging Strutturato**: Diversi livelli (DEBUG, INFO, WARN, ERROR)

### Stati Error

- **FailureReason**: Categorizzazione errori (ringbuffer underrun, I2S write, etc.)
- **Recovery Counters**: Tracking tentativi recovery
- **Circuit Breaker**: Evita loop infiniti

## Performance Characteristics

### Latenza Tipica

- **File locale**: < 50ms startup
- **HTTP stream**: 1-3 secondi (primo chunk)
- **Seek file**: < 100ms
- **Seek timeshift**: 200-500ms (dipende da storage)

### Throughput

- **MP3 128kbps**: ~15x realtime (decodifica)
- **WAV 44.1kHz**: ~3x realtime
- **Memory bandwidth**: Sufficiente per stereo 16-bit

### CPU Usage

- **Idle**: < 5%
- **Riproduzione**: 15-30% (dipende formato)
- **Timeshift**: +10-20% (download task)

## Estensibilità

### Aggiungere Nuovo Decoder

1. Implementare `AudioDecoder` subclass
2. Registrare in `AudioDecoderFactory::create()`
3. Aggiungere detection in `AudioStream`

### Aggiungere Nuova DataSource

1. Implementare `IDataSource` interface
2. Aggiungere detection in `AudioPlayer::select_source()`
3. Gestire cleanup appropriato

### Aggiungere Nuovo Codec Hardware

1. Implementare interfaccia I2S standard
2. Configurare pin mapping
3. Testare sincronizzazione clock

## Sicurezza e Robustezza

### Input Validation

- **URI Sanitization**: Controllo paths e URL
- **Buffer Bounds**: Protezione overrun
- **Type Safety**: Strong typing dove possibile

### Resource Management

- **RAII**: Automatic cleanup risorse
- **Leak Prevention**: Smart pointers per oggetti pesanti
- **Timeout Protection**: Limiti su operazioni bloccanti

### Thread Safety

- **Mutex Protection**: Buffer condivisi
- **Atomic Operations**: Per stati volatili
- **Task Affinity**: Predicibilità scheduling
