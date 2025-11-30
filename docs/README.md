# openESPaudio Documentation

Una libreria audio avanzata per ESP32 con supporto per streaming HTTP, timeshift e riproduzione locale.

## Navigazione Rapida

| Documento | Descrizione |
|-----------|-------------|
| [Quick Start](QUICK_START.md) | Guida per iniziare in 5 minuti |
| [API Reference](api/API_REFERENCE.md) | Riferimento completo delle API |
| [Timeshift Guide](guides/TIMESHIFT_GUIDE.md) | Guida al timeshift e streaming |
| [Troubleshooting](guides/TROUBLESHOOTING.md) | Risoluzione problemi comuni |
| [Architecture](ARCHITECTURE.md) | Architettura interna |

## Features Principali

- **Riproduzione Locale**: File MP3/WAV da LittleFS o SD card
- **Streaming HTTP**: Radio online con buffering intelligente
- **Timeshift**: Pausa/riavvolgi stream live (PSRAM veloce o SD illimitato)
- **Seek Temporale**: Salto preciso nei file e stream bufferizzati
- **Controllo Volume**: Regolazione 0-100%
- **Auto-Pausa**: Pausa automatica durante buffering lento

## Esempio Rapido

```cpp
#include <openESPaudio.h>

AudioPlayer player;

void setup() {
  // Riproduzione file locale
  player.select_source("/music/song.mp3");
  player.arm_source();
  player.start();

  // O streaming con timeshift
  auto* ts = new TimeshiftManager();
  ts->setStorageMode(StorageMode::PSRAM_ONLY);
  ts->open("http://radio.example.com/stream.mp3");
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

## Requisiti Hardware

- ESP32-S3 con PSRAM (raccomandato per timeshift)
- Codec I2S (ES8311 o compatibile)
- WiFi per streaming HTTP
- SD card opzionale per buffering esteso

## Installazione

Vedi [Quick Start](QUICK_START.md) per dettagli completi.

---

*Documentazione aggiornata per le nuove funzionalità di cambio runtime PSRAM/SD e salvataggio registrazioni.*
