# openESPaudio

Libreria audio avanzata per ESP32 con supporto per file locali, streaming HTTP e timeshift.

## 🚀 Quick Start

```cpp
#include <openESPaudio.h>

AudioPlayer player;

void setup() {
  // File locale
  player.select_source("/music.mp3");
  player.arm_source();
  player.start();

  // O radio con timeshift
  auto* ts = new TimeshiftManager();
  ts->open("http://radio.stream/mp3");
  ts->start();
  player.select_source(std::unique_ptr<IDataSource>(ts));
  player.arm_source();
  player.start();
}

void loop() {
  player.tick_housekeeping(); // Richiesto!
  delay(10);
}
```

## ✨ Features

- **Riproduzione Locale**: MP3/WAV da LittleFS o SD card
- **Streaming HTTP**: Radio online con buffering intelligente
- **Timeshift**: Pausa/riavvolgi stream live (PSRAM veloce o SD illimitato)
- **Seek Temporale**: Salto preciso in file e buffer
- **Controllo Playback**: Play/pause, volume, seek
- **Cambio Storage Runtime**: PSRAM ↔ SD senza interruzioni

## 📚 Documentazione

| Guida | Descrizione |
|-------|-------------|
| [Quick Start](docs/QUICK_START.md) | Per iniziare in 5 minuti |
| [API Reference](docs/api/API_REFERENCE.md) | Riferimento API completo |
| [Timeshift Guide](docs/guides/TIMESHIFT_GUIDE.md) | Guida al timeshift |
| [Troubleshooting](docs/guides/TROUBLESHOOTING.md) | Risoluzione problemi |
| [Architettura](docs/ARCHITECTURE.md) | Dettagli interni |

## 📦 Installazione

### PlatformIO
```ini
lib_deps = https://github.com/yourusername/openESPaudio.git
```

### Arduino IDE
Scarica ZIP → Sketch → Include Library → Add .ZIP Library

## 🔧 Requisiti Hardware

- **ESP32-S3** con PSRAM (raccomandato)
- **Codec I2S** (ES8311, PCM5102, etc.)
- **WiFi** per streaming
- **SD card** opzionale per buffer esteso

## 📝 Esempi

- `examples/1_BasicFilePlayback/` - Riproduzione file base
- `examples/2_RadioTimeshift/` - Streaming con timeshift
- `examples/3_AdvancedControl/` - Controlli avanzati

## 📋 API Principali

```cpp
// AudioPlayer
player.select_source("/file.mp3");
player.arm_source();
player.start();
player.set_volume(75);
player.request_seek(30);

// TimeshiftManager
auto* ts = new TimeshiftManager();
ts->setStorageMode(StorageMode::PSRAM_ONLY);
ts->open("http://stream.mp3");
ts->switchStorageMode(StorageMode::SD_CARD); // Runtime switch
```

## 🐛 Troubleshooting

- **No audio**: Verifica pin I2S e codec inizializzato
- **WiFi fail**: Connetti WiFi prima di aprire stream
- **Seek non funziona**: Controlla `is_seekable()`
- **Out of memory**: Abilita PSRAM nella configurazione

Vedi [Troubleshooting](docs/guides/TROUBLESHOOTING.md) per dettagli.

## 📄 License

MIT License - Copyright (c) 2025 rederyk

## 🤝 Contributing

Contributions welcome! Vedi [docs](docs/) per dettagli sviluppo.
