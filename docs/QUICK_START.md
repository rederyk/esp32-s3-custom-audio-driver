# Quick Start - openESPaudio

Guida per iniziare con openESPaudio in 5 minuti.

## 1. Installazione

### PlatformIO (Raccomandato)

1. Aggiungi al tuo `platformio.ini`:
```ini
[env:esp32-s3-devkitm-1]
platform = espressif32
board = esp32-s3-devkitm-1
framework = arduino
lib_deps =
    https://github.com/yourusername/openESPaudio.git
build_flags =
    -DBOARD_HAS_PSRAM
    -mfix-esp32-psram-cache-issue
```

2. Clona nella cartella `lib` del progetto:
```bash
cd your_project/lib
git clone https://github.com/yourusername/openESPaudio.git
```

### Arduino IDE

1. Scarica ZIP da GitHub
2. Arduino IDE → Sketch → Include Library → Add .ZIP Library

## 2. Configurazione Hardware

- **ESP32-S3** con almeno 2MB PSRAM
- **Codec I2S** (ES8311, PCM5102, MAX98357A)
- **SD card** opzionale per buffering esteso
- **WiFi** per streaming HTTP

## 3. Primo Esempio: Riproduzione File Locale

```cpp
#include <Arduino.h>
#include <LittleFS.h>
#include <openESPaudio.h>

AudioPlayer player;

void setup() {
  Serial.begin(115200);

  // Inizializza filesystem
  LittleFS.begin();

  // Carica e avvia riproduzione
  player.select_source("/sample.mp3");
  player.arm_source();
  player.start();
}

void loop() {
  player.tick_housekeeping(); // CRITICO: chiamare regolarmente
  delay(10);
}
```

## 4. Streaming Radio con Timeshift

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <openESPaudio.h>

AudioPlayer player;

void setup() {
  Serial.begin(115200);

  // Connetti WiFi
  WiFi.begin("SSID", "PASSWORD");
  while (WiFi.status() != WL_CONNECTED) delay(500);

  // Crea timeshift manager
  auto* ts = new TimeshiftManager();
  ts->setStorageMode(StorageMode::PSRAM_ONLY); // Veloce, ~2min buffer

  // Apri stream
  ts->open("http://stream.example.com/radio.mp3");
  ts->start();

  // Aspetta primo chunk
  while (ts->buffered_bytes() == 0) delay(100);

  // Avvia riproduzione
  player.select_source(std::unique_ptr<IDataSource>(ts));
  player.arm_source();
  player.start();
}

void loop() {
  player.tick_housekeeping();
  delay(10);
}
```

## 5. Controlli Base

```cpp
// Playback
player.start();           // Avvia
player.stop();            // Ferma
player.toggle_pause();    // Pausa/riprendi

// Volume e seek
player.set_volume(75);    // 0-100%
player.request_seek(30);  // Vai al secondo 30

// Status
Serial.printf("Posizione: %u/%u sec\n",
  player.current_position_sec(),
  player.total_duration_sec());
```

## 6. Debug e Troubleshooting

```cpp
#include <logger.h>

void setup() {
  set_log_level(LogLevel::DEBUG); // INFO, WARN, ERROR
}
```

**Problemi comuni:**
- **No audio**: Verifica pin I2S e codec inizializzato
- **WiFi fallisce**: Assicurati connessione prima di aprire stream
- **Seek non funziona**: Controlla se sorgente è seekable

Vedi [Troubleshooting](guides/TROUBLESHOOTING.md) per dettagli.

## Esempi Complet

Guarda gli esempi nella cartella `examples/`:
- `1_BasicFilePlayback/` - Riproduzione file base
- `2_RadioTimeshift/` - Streaming con timeshift
- `3_AdvancedControl/` - Controlli completi via serial

## Prossimi Passi

- Leggi [API Reference](api/API_REFERENCE.md) per metodi avanzati
- Scopri [Timeshift Guide](guides/TIMESHIFT_GUIDE.md) per funzionalità avanzate
- Consulta [Architecture](ARCHITECTURE.md) per comprensione interna
