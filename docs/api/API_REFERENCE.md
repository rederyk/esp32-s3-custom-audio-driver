# API Reference - openESPaudio

Riferimento completo delle API pubbliche.

## AudioPlayer

Classe principale per controllo riproduzione audio.

### Costruttore

```cpp
AudioPlayer player;  // Usa configurazione default
AudioPlayer player(custom_config);  // Configurazione custom
```

### Metodi Principali

#### Selezione Sorgente

```cpp
bool select_source(const char* uri, SourceType hint = SourceType::LITTLEFS);
bool select_source(std::unique_ptr<IDataSource> source);
```

**Parametri:**
- `uri`: Path file o URL HTTP
- `hint`: Tipo sorgente (LITTLEFS, SD_CARD, HTTP_STREAM)
- `source`: Puntatore a IDataSource custom (es. TimeshiftManager)

**Ritorna:** `true` se selezione riuscita

#### Controllo Riproduzione

```cpp
bool arm_source();        // Carica e prepara sorgente
void start();             // Avvia riproduzione
void stop();              // Ferma riproduzione
void toggle_pause();      // Toggle pausa
void set_pause(bool pause); // Imposta pausa diretta
```

#### Seek e Volume

```cpp
void set_volume(int vol_pct);   // 0-100%
void request_seek(int seconds); // Seek a secondi
```

#### Status e Info

```cpp
PlayerState state() const;              // STATO: STOPPED, PLAYING, PAUSED, ENDED, ERROR
uint32_t current_position_sec() const;  // Posizione corrente secondi
uint32_t total_duration_sec() const;    // Durata totale secondi
uint32_t current_position_ms() const;   // Posizione corrente millisecondi
uint32_t total_duration_ms() const;     // Durata totale millisecondi
bool is_playing() const;                // true se in riproduzione
const Metadata& metadata() const;       // Metadati ID3
SourceType source_type() const;         // Tipo sorgente attuale
const char* current_uri() const;        // URI sorgente attuale
uint32_t current_bitrate() const;       // Bitrate corrente kbps
AudioFormat current_format() const;     // Formato: MP3, WAV
```

#### Housekeeping

```cpp
void tick_housekeeping(); // CRITICO: chiamare in loop() regolarmente
```

#### Callbacks

```cpp
void set_callbacks(const PlayerCallbacks& cb);

struct PlayerCallbacks {
    void (*on_start)(const char* path) = nullptr;
    void (*on_stop)(const char* path, PlayerState state) = nullptr;
    void (*on_end)(const char* path) = nullptr;
    void (*on_error)(const char* path, const char* detail) = nullptr;
    void (*on_metadata)(const Metadata& meta, const char* path) = nullptr;
    void (*on_progress)(uint32_t pos_ms, uint32_t dur_ms) = nullptr;
};
```

### Stati Player

```cpp
enum class PlayerState {
    STOPPED,  // Nessuna riproduzione
    PLAYING,  // In riproduzione
    PAUSED,   // In pausa
    ENDED,    // Riproduzione completata
    ERROR     // Errore occorso
};
```

### Tipi Sorgente

```cpp
enum class SourceType {
    LITTLEFS,     // File da LittleFS (/file.mp3)
    SD_CARD,      // File da SD (/sd/file.mp3)
    HTTP_STREAM   // Stream HTTP (http://...)
};
```

## TimeshiftManager

Gestisce streaming HTTP con buffer timeshift.

### Costruttore

```cpp
TimeshiftManager* ts = new TimeshiftManager();
```

### Configurazione

```cpp
void setStorageMode(StorageMode mode);  // PSRAM_ONLY o SD_CARD
StorageMode getStorageMode() const;
```

### Controllo Stream

```cpp
bool open(const char* uri);      // Apri URL stream
bool start();                    // Avvia download task
void stop();                     // Ferma download
void pause_recording();          // Pausa registrazione buffer
void resume_recording();         // Riprendi registrazione
bool switchStorageMode(StorageMode new_mode); // Cambio runtime PSRAM↔SD
```

### Status Buffer

```cpp
size_t buffered_bytes() const;          // Byte bufferizzati
size_t total_downloaded_bytes() const;  // Byte totali scaricati
float buffer_duration_seconds() const;  // Durata buffer secondi
bool is_recording_paused() const;       // true se registrazione in pausa
```

### Seek Temporale

```cpp
uint32_t total_duration_ms() const;     // Durata totale disponibile
uint32_t current_position_ms() const;   // Posizione corrente
size_t seek_to_time(uint32_t target_ms); // Seek a timestamp
```

### Auto-Pausa Buffering

```cpp
void set_auto_pause_callback(std::function<void(bool)> callback);
void set_auto_pause_margin(uint32_t delay_ms, size_t min_chunks);
```

**Esempio:**
```cpp
ts->set_auto_pause_callback([](bool should_pause) {
    player.set_pause(should_pause);
});
ts->set_auto_pause_margin(1500, 2); // 1.5s delay, 2 chunk min
```

### Modalità Storage

```cpp
enum class StorageMode {
    SD_CARD,    // Lento, buffer illimitato, usa SD card
    PSRAM_ONLY  // Veloce, ~2min buffer, usa PSRAM
};
```

## SdCardDriver

Singleton per accesso SD card.

### Ottieni Istanza

```cpp
auto& sd = SdCardDriver::getInstance();
```

### Metodi

```cpp
bool begin();                    // Inizializza SD
bool isMounted() const;          // true se montata
String lastError() const;        // Ultimo errore
uint64_t totalBytes() const;     // Capacità totale
uint64_t usedBytes() const;      // Spazio usato
String cardTypeString() const;   // Tipo carta
vector<DirectoryEntry> listDirectory(const char* path);
```

## Utility Globali

### Logging

```cpp
#include <logger.h>

void set_log_level(LogLevel level);

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};
```

### Configurazione Audio

```cpp
struct AudioConfig {
    uint32_t sample_rate = 44100;
    uint8_t channels = 2;
    uint8_t bits_per_sample = 16;
    uint32_t i2s_buffer_size = 1024;
    uint8_t i2s_buffer_count = 8;
};

AudioConfig default_audio_config();
```

### Metadati

```cpp
struct Metadata {
    String title;
    String artist;
    String album;
    uint32_t year = 0;
    String genre;
    uint32_t duration_ms = 0;
    uint32_t bitrate_kbps = 0;
};
```

### Formati Audio

```cpp
enum class AudioFormat {
    UNKNOWN,
    MP3,
    WAV
};
```

## Esempi API

### Riproduzione File con Seek

```cpp
player.select_source("/music/song.mp3");
player.arm_source();
player.start();

// Seek a 30 secondi
player.request_seek(30);

// Volume 75%
player.set_volume(75);

// Status
if (player.is_playing()) {
    printf("Pos: %u/%u sec\n",
        player.current_position_sec(),
        player.total_duration_sec());
}
```

### Timeshift con Auto-Pausa

```cpp
auto* ts = new TimeshiftManager();
ts->setStorageMode(StorageMode::PSRAM_ONLY);
ts->open("http://radio.example.com/stream.mp3");
ts->start();

// Auto-pausa quando buffer insufficiente
ts->set_auto_pause_callback([](bool pause) {
    player.set_pause(pause);
});

// Attendi primo chunk
while (ts->buffered_bytes() == 0) delay(100);

player.select_source(std::unique_ptr<IDataSource>(ts));
player.arm_source();
player.start();
```

### Cambio Storage Runtime

```cpp
// Durante riproduzione, cambia da PSRAM a SD
const IDataSource* source = player.data_source();
if (source->type() == SourceType::HTTP_STREAM) {
    TimeshiftManager* ts = static_cast<TimeshiftManager*>(
        const_cast<IDataSource*>(source));
    if (ts->switchStorageMode(StorageMode::SD_CARD)) {
        LOG_INFO("Switched to SD card storage");
    }
}
