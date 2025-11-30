# Timeshift Guide - openESPaudio

Guida completa al timeshift: pausa/riavvolgi stream live con buffering PSRAM/SD.

## Concetto Base

Il timeshift permette di:
- **Pausare** uno stream radio live
- **Riavvolgere** per riascoltare parti precedenti
- **Saltare** avanti nel buffer disponibile
- Buffer in **PSRAM** (veloce, ~2 minuti) o **SD card** (lento, illimitato)

## Setup Base

```cpp
#include <openESPaudio.h>

AudioPlayer player;
TimeshiftManager* ts;

void setup() {
  // Connetti WiFi prima
  WiFi.begin("SSID", "PASSWORD");
  while (WiFi.status() != WL_CONNECTED) delay(500);

  // Crea timeshift manager
  ts = new TimeshiftManager();

  // Configura storage
  ts->setStorageMode(StorageMode::PSRAM_ONLY); // Veloce, limitato

  // Apri stream
  ts->open("http://radio.example.com/stream.mp3");
  ts->start();

  // Attendi buffering iniziale
  while (ts->buffered_bytes() == 0) delay(100);

  // Collega a player
  player.select_source(std::unique_ptr<IDataSource>(ts));
  player.arm_source();
  player.start();
}

void loop() {
  player.tick_housekeeping();
  delay(10);
}
```

## Modalità Storage

### PSRAM_ONLY (Raccomandato per velocità)

```cpp
ts->setStorageMode(StorageMode::PSRAM_ONLY);
```

**Pro:**
- Accesso **velocissimo** (no I/O disco)
- **2 minuti** buffer tipico
- Nessun rumore seek SD

**Contro:**
- Buffer limitato (~2MB PSRAM)
- Richiede ESP32-S3 con PSRAM

### SD_CARD (Buffer illimitato)

```cpp
ts->setStorageMode(StorageMode::SD_CARD);
```

**Pro:**
- Buffer **illimitato** (finché c'è spazio SD)
- Funziona su ESP32 senza PSRAM

**Contro:**
- Più **lento** per seek/riavvolgimento
- Rumoroso durante scrittura
- Maggiore consumo corrente

## Cambio Storage Runtime

**NUOVA FEATURE:** Cambia modalità durante la riproduzione senza interrompere.

```cpp
// Da PSRAM a SD (per buffer più lungo)
const IDataSource* source = player.data_source();
if (source->type() == SourceType::HTTP_STREAM) {
    TimeshiftManager* ts = static_cast<TimeshiftManager*>(
        const_cast<IDataSource*>(source));

    if (ts->switchStorageMode(StorageMode::SD_CARD)) {
        LOG_INFO("Passato a SD card - buffer ora illimitato");
    }
}
```

**Quando cambiare:**
- **PSRAM → SD**: Quando vuoi buffer più lungo di 2 minuti
- **SD → PSRAM**: Per velocità e non sprecare scritture sd 

## Controllo Playback

### Seek Temporale

```cpp
// Riavvolgi di 30 secondi
uint32_t current = player.current_position_sec();
player.request_seek(current - 30);

// Salta avanti di 1 minuto
player.request_seek(current + 60);

// Vai all'inizio del buffer disponibile
player.request_seek(0);
```

### Pausa e Resume

```cpp
player.toggle_pause();  // Toggle
player.set_pause(true); // Pausa
player.set_pause(false); // Resume
```

**Nota:** Durante pausa, il buffer continua ad accumularsi.

## Auto-Pausa Buffering

Pausa automatica quando il buffer è insufficiente (connessioni lente).

```cpp
// Imposta callback auto-pausa
ts->set_auto_pause_callback([](bool should_pause) {
    player.set_pause(should_pause);
    if (should_pause) {
        LOG_INFO("Buffer insufficiente - pausa automatica");
    } else {
        LOG_INFO("Buffer OK - resume riproduzione");
    }
});

// Configura margini (opzionale)
ts->set_auto_pause_margin(1500, 2); // 1.5s delay, minimo 2 chunk
```

**Parametri:**
- `delay_ms`: Millisecondi di attesa prima di riprendere
- `min_chunks`: Chunk minimi necessari per riprendere

## Monitoraggio Buffer

```cpp
// Status in tempo reale
size_t buffered = ts->buffered_bytes();
size_t downloaded = ts->total_downloaded_bytes();
float duration_sec = ts->buffer_duration_seconds();

// Info posizione
uint32_t current_ms = player.current_position_ms();
uint32_t total_ms = player.total_duration_ms();

LOG_INFO("Buffer: %u KB (%u sec) | Pos: %u/%u sec",
    buffered / 1024, (uint32_t)duration_sec,
    current_ms / 1000, total_ms / 1000);
```

## Gestione Registrazioni

### Salvataggio Registrazioni

**NUOVA FEATURE:** Salva registrazioni invece di cancellarle all'avvio.

```cpp
// Durante riproduzione, marca chunk per esportazione
ts->mark_chunk_for_export(chunk_id);

// Alla fine, sposta i chunk marcati in cartella export
ts->cleanup_timeshift_directory(); // Sposta marcati, cancella altri
```

### Estrazione MP3

```cpp
// Estrai registrazione completa (da implementare)
// player.extract_recording("/sd/recordings/radio_2025.mp3");
```

**Workflow tipico:**
1. Ascolta radio con timeshift attivo
2. Quando senti qualcosa di interessante: `mark_chunk_for_export()`
3. Alla fine: `cleanup_timeshift_directory()` salva i chunk marcati

## Troubleshooting Timeshift

### "Timeout waiting for first chunk"
```
CAUSA: Stream non raggiungibile o WiFi lento
SOLUZIONE:
- Verifica URL stream in browser
- Controlla connessione WiFi
- Aumenta timeout in codice
```

### "Seek non funziona"
```
CAUSA: Buffer insufficiente o posizione oltre limite
SOLUZIONE:
- Controlla ts->buffered_bytes() > 0
- Seek solo entro total_duration_ms()
- Attendi più buffering per seek avanti
```

### "Audio si interrompe durante seek"
```
CAUSA: Buffer pieno in PSRAM mode
SOLUZIONE:
- Passa a SD_CARD mode
- Riduci frequenza seek
- Aumenta PSRAM se possibile
```

### "Switch storage fallisce"
```
CAUSA: Spazio SD insufficiente o errore I/O
SOLUZIONE:
- Verifica spazio SD libero
- Controlla SdCardDriver::getInstance().isMounted()
- Log errore dettagliato
```

## Performance Tips

1. **Usa PSRAM** per contenuti brevi/interattivi
2. **SD card** per sessioni lunghe
3. **Auto-pausa** per connessioni instabili
4. **Monitora buffer** regolarmente
5. **Switch runtime** quando necessario

## Esempi Avanzati

### Timeshift con Controlli Seriali

```cpp
void handle_serial_commands() {
    if (Serial.available()) {
        char cmd = Serial.read();
        switch(cmd) {
            case 'p': player.toggle_pause(); break;
            case '[': { // Rewind 30s
                uint32_t pos = player.current_position_sec();
                player.request_seek(pos > 30 ? pos - 30 : 0);
                break;
            }
            case 's': { // Switch storage mode
                StorageMode current = ts->getStorageMode();
                StorageMode next = (current == StorageMode::PSRAM_ONLY) ?
                    StorageMode::SD_CARD : StorageMode::PSRAM_ONLY;
                ts->switchStorageMode(next);
                break;
            }
        }
    }
}
```

### Timeshift con Display OLED

```cpp
void update_display() {
    display.clearDisplay();

    // Progress bar
    uint32_t pos = player.current_position_ms();
    uint32_t total = player.total_duration_ms();
    int bar_width = (pos * 128) / total;
    display.fillRect(0, 0, bar_width, 8, WHITE);

    // Status text
    display.setCursor(0, 16);
    display.printf("%u/%u sec", pos/1000, total/1000);

    // Buffer info
    display.setCursor(0, 32);
    display.printf("Buffer: %u KB", ts->buffered_bytes() / 1024);

    display.display();
}
```

Vedi esempi completi in `examples/2_RadioTimeshift/` e `examples/3_AdvancedControl/`.
