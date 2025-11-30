# openESPaudio Library Guide

## Da Applicazione a Libreria

Questa guida spiega come il progetto è stato trasformato da applicazione standalone a libreria riutilizzabile.

## Struttura del Progetto

```
openESPaudio/
├── library.properties          # Metadati della libreria
├── LICENSE                     # Licenza MIT
├── README.md                   # Documentazione principale
├── keywords.txt               # Syntax highlighting per Arduino IDE
├── src/
│   ├── openESPaudio.h         # Header principale della libreria
│   ├── audio_player.h/cpp     # Controller di riproduzione
│   ├── timeshift_manager.h/cpp # Gestione streaming con timeshift
│   ├── data_source*.h         # Sorgenti dati (LittleFS, SD, HTTP)
│   ├── drivers/               # Driver hardware (SD card, I2S, codec)
│   └── ...                    # Altri componenti interni
└── examples/
    ├── 1_BasicFilePlayback/   # Esempio base
    ├── 2_RadioTimeshift/      # Esempio streaming
    └── 3_AdvancedControl/     # Esempio avanzato
```

## Principi Chiave della Libreria

### 1. Separazione delle Responsabilità

**La libreria fornisce:**
- Classi per la riproduzione audio (`AudioPlayer`)
- Gestione streaming con timeshift (`TimeshiftManager`)
- Supporto per diverse sorgenti dati
- API pulita e documentata

**L'applicazione utente gestisce:**
- Connessione WiFi
- Inizializzazione filesystem (LittleFS, SD card)
- Interfaccia utente (comandi seriali, pulsanti, display)
- Logica applicativa specifica

### 2. Gestione WiFi

❌ **La libreria NON gestisce il WiFi**

La libreria non chiama mai `WiFi.begin()` o gestisce la connessione. Questo permette all'utente di:
- Usare qualsiasi metodo di connessione (WiFiManager, SmartConfig, ecc.)
- Integrare la libreria in applicazioni che già usano WiFi
- Gestire riconnessioni secondo le proprie necessità

✅ **Esempio corretto:**

```cpp
void setup() {
  // L'utente gestisce WiFi
  WiFi.begin("SSID", "PASSWORD");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  // Ora può usare lo streaming
  player.select_source("http://...");
}
```

La libreria verifica internamente se WiFi è connesso quando necessario e restituisce un errore se non lo è.

### 3. Gestione Filesystem

Anche i filesystem devono essere inizializzati dall'utente:

```cpp
void setup() {
  // Per file da LittleFS
  LittleFS.begin();

  // Per file da SD card
  SdCardDriver::getInstance().begin();

  // Ora si possono riprodurre file
  player.select_source("/music/song.mp3");
}
```

## Migrazione dalla Vecchia Applicazione

### File `main.cpp`

Il vecchio `main.cpp` (ora rinominato in `main.cpp.old`) conteneva:
- Gestione comandi seriali
- Credenziali WiFi hardcoded
- Inizializzazione hardware
- Loop principale con log periodici

Questa logica è stata spostata negli **esempi**, specialmente in `3_AdvancedControl.ino`.

### Dove Trovare le Funzionalità

| Vecchia posizione | Nuova posizione |
|------------------|-----------------|
| `handle_command_string()` | `examples/3_AdvancedControl/` |
| `start_timeshift_radio()` | `examples/2_RadioTimeshift/` |
| Credenziali WiFi | Sketch utente (esempi) |
| Inizializzazione hardware | Sketch utente (esempi) |
| Loop con status log | `examples/1_BasicFilePlayback/` |

## Come Usare la Libreria

### Installazione

**Metodo 1: PlatformIO (consigliato)**
```ini
lib_deps =
    https://github.com/yourusername/openESPaudio.git
```

**Metodo 2: Locale (per sviluppo)**
```bash
cd your_project/lib
git clone /path/to/openESPaudio
```

### Configurazione PlatformIO

Il tuo `platformio.ini` deve contenere i flag necessari per ESP32-S3 e PSRAM:

```ini
[env:your_board]
platform = espressif32
board = esp32-s3-devkitm-1
framework = arduino
build_flags =
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DBOARD_HAS_PSRAM
    -mfix-esp32-psram-cache-issue
board_build.filesystem = littlefs
board_build.partitions = default_16MB.csv
board_build.arduino.memory_type = qio_opi
board_build.psram.enable = true
```

### Esempio Minimale

```cpp
#include <openESPaudio.h>

AudioPlayer player;

void setup() {
  Serial.begin(115200);
  LittleFS.begin();

  player.select_source("/audio.mp3");
  player.arm_source();
  player.start();
}

void loop() {
  player.tick_housekeeping();  // ESSENZIALE!
  delay(10);
}
```

## API Pubblica vs Implementazione Interna

### API Pubblica (cosa l'utente usa)

```cpp
#include <openESPaudio.h>  // Include tutto il necessario

AudioPlayer player;
TimeshiftManager ts;
SdCardDriver::getInstance();
```

### Implementazione Interna (non usare direttamente)

Gli utenti NON dovrebbero includere direttamente:
- `audio_stream.h`
- `audio_decoder.h`
- `mp3_decoder.h`
- File interni di implementazione

Tutti i componenti necessari sono esposti attraverso `openESPaudio.h`.

## Best Practices

### 1. Chiamare `tick_housekeeping()` Regolarmente

```cpp
void loop() {
  player.tick_housekeeping();  // Chiamata OBBLIGATORIA
  // ... altra logica
  delay(10);  // Non bloccare troppo a lungo
}
```

### 2. Gestire gli Stati

```cpp
if (player.state() == PlayerState::PLAYING) {
  // In riproduzione
} else if (player.state() == PlayerState::ENDED) {
  // Finito - ricaricare o cambiare sorgente
  player.select_source("/next_song.mp3");
  player.arm_source();
  player.start();
}
```

### 3. Timeshift con Auto-Pause

```cpp
ts->set_auto_pause_callback([](bool should_pause) {
  player.set_pause(should_pause);
  if (should_pause) {
    Serial.println("Buffering...");
  }
});
```

### 4. Controllo Errori

```cpp
if (!player.select_source(path)) {
  Serial.println("Source selection failed");
  return;
}

if (!player.arm_source()) {
  Serial.println("Failed to load source");
  return;
}
```

## Domande Frequenti

### Q: Posso usare la libreria se ho già una connessione WiFi?
**A:** Sì! La libreria non tocca mai la tua configurazione WiFi. Assicurati solo che WiFi sia connesso prima di aprire stream HTTP.

### Q: Come compilo gli esempi?
**A:** Con PlatformIO:
```bash
# Copia l'esempio nella tua cartella src
cp -r examples/1_BasicFilePlayback/1_BasicFilePlayback.ino src/main.cpp
pio run -t upload
```

### Q: Posso usare la libreria con Arduino IDE?
**A:** Sì, installa come libreria ZIP. Ma PlatformIO è fortemente consigliato per la gestione delle dipendenze.

### Q: Il timeshift funziona senza SD card?
**A:** Sì, usa `StorageMode::PSRAM_ONLY`. Richiede ESP32 con PSRAM (minimo 2MB). Buffer limitato a ~2 minuti.

### Q: Come debug problemi audio?
**A:** Abilita logging dettagliato:
```cpp
set_log_level(LogLevel::DEBUG);
```

## Contribuire

Se vuoi contribuire alla libreria:

1. Fork il repository
2. Crea un branch per la feature
3. Mantieni la separazione libreria/applicazione
4. Aggiungi esempi se necessario
5. Documenta l'API pubblica
6. Invia una pull request

## Supporto

- **Bug**: Apri un issue su GitHub
- **Domande**: Usa GitHub Discussions
- **Esempi**: Vedi cartella `examples/`

---

**Buon coding! 🎵**
