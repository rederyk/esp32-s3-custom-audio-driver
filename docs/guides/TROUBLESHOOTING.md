# Troubleshooting - openESPaudio

Guida alla risoluzione dei problemi più comuni.

## Audio Non Si Sente

### Sintomi
- Nessun suono dagli speaker
- LED codec acceso ma silenzioso

### Possibili Cause e Soluzioni

#### 1. Configurazione I2S Errata
```
SOLUZIONE: Verifica pin I2S nel tuo codice
- BCLK, WS, DOUT collegati correttamente
- Codec inizializzato prima di AudioPlayer
- Controlla schemi del tuo codec (ES8311, PCM5102, etc.)
```

#### 2. Volume a Zero
```
SOLUZIONE: Imposta volume esplicito
player.set_volume(75); // 0-100%
```

#### 3. Codec Non Inizializzato
```
SOLUZIONE: Verifica inizializzazione codec
// Prima di creare AudioPlayer
codec.begin();
codec.setVolume(75);
```

#### 4. Alimentazione Insufficiente
```
SOLUZIONE:
- Verifica 3.3V stabile per ESP32
- Codec audio potrebbe richiedere più corrente
- Aggiungi condensatori di filtro
```

## WiFi Streaming Fallisce

### Sintomi
- "Failed to open HTTP stream"
- Timeout connessione

### Soluzioni

#### 1. WiFi Non Connesso
```
SOLUZIONE: Connetti WiFi PRIMA di aprire stream
WiFi.begin("SSID", "PASSWORD");
while (WiFi.status() != WL_CONNECTED) delay(500);
// ORA puoi usare ts->open("http://...")
```

#### 2. URL Non Valido
```
SOLUZIONE:
- Testa URL in browser del PC
- Verifica protocollo (http://, non https://)
- Alcuni stream richiedono headers specifici
```

#### 3. Firewall/Proxy
```
SOLUZIONE:
- Prova da rete diversa
- Verifica impostazioni firewall
- Alcuni stream bloccano ESP32 user-agent
```

#### 4. Memoria Insufficiente
```
SOLUZIONE: Monitora heap libero
LOG_INFO("Heap libero: %u bytes", esp_get_free_heap_size());
- Chiudi altre connessioni prima
- Usa PSRAM se disponibile
```

## Seek Non Funziona

### Sintomi
- Seek ignorato o posizione errata
- "Cannot seek" nei log

### Cause Comuni

#### 1. Sorgente Non Seekable
```
CAUSA: Alcuni stream live non supportano seek
SOLUZIONE: Controlla player.data_source()->is_seekable()
```

#### 2. Buffer Insufficiente
```
CAUSA: Seek oltre dati bufferizzati
SOLUZIONE:
- Per timeshift: attendi più buffering
- Controlla total_duration_ms() prima di seek
```

#### 3. File Corrotto
```
CAUSA: MP3 malformato o interrotto
SOLUZIONE:
- Verifica file su PC con player normale
- Usa WAV per test (sempre seekable)
```

## Timeshift Problemi

### "Timeout waiting for first chunk"
```
CAUSA: Stream lento o irraggiungibile
SOLUZIONE:
- Aumenta timeout: while(ts->buffered_bytes() == 0 && millis() < start+30000) delay(100);
- Verifica URL stream
- Controlla velocità connessione WiFi
```

### "Switch storage mode failed"
```
CAUSA: Spazio SD insufficiente o errori I/O
SOLUZIONE:
- Verifica SD montata: SdCardDriver::getInstance().isMounted()
- Controlla spazio libero: sd.usedBytes() vs sd.totalBytes()
- Log errore: sd.lastError()
```

### Buffer Si Riempie Troppo Lentamente
```
CAUSA: Connessione lenta o bitrate alto
SOLUZIONE:
- Usa auto-pausa: ts->set_auto_pause_callback(...)
- Passa a bitrate più basso se disponibile
- Migliora segnale WiFi
```

### Seek Lento in SD Mode
```
CAUSA: I/O disco durante riproduzione
SOLUZIONE:
- Passa a PSRAM mode per contenuti brevi
- Riduci frequenza seek
- Usa SSD invece di SD card se possibile
```

## Problemi Memoria

### "Out of memory" o Crash
```
CAUSA: Heap esaurito
SOLUZIONE:
- Abilita PSRAM nel tuo board config
- Monitora memoria: esp_get_free_heap_size()
- Riduci buffer sizes se necessario
- Chiudi risorse non usate
```

### PSRAM Non Disponibile
```
CAUSA: Board senza PSRAM o non abilitato
SOLUZIONE:
- Verifica board supporta PSRAM (ESP32-S3)
- Aggiungi build_flags per PSRAM
- Usa SD card come fallback
```

## SD Card Problemi

### "SD card not mounted"
```
CAUSA: Connessioni errate o scheda difettosa
SOLUZIONE:
- Verifica pin SD: MOSI, MISO, SCK, CS
- Prova scheda diversa
- Formatta come FAT32
- Controlla alimentazione stabile
```

### File Non Trovati
```
CAUSA: Path case-sensitive o directory errata
SOLUZIONE:
- Usa paths assoluti: "/music/song.mp3"
- Per SD: "/sd/music/song.mp3"
- Lista directory: sd.listDirectory("/")
```

### Scrittura Lenta
```
CAUSA: SD card lenta o frammentata
SOLUZIONE:
- Usa SD card Class 10 o superiore
- Deframmenta o formatta scheda
- Riduci dimensioni chunk se necessario
```

## Problemi Codec Audio

### Distorsione Audio
```
CAUSA: Configurazione sample rate errata
SOLUZIONE:
- Verifica codec supporta 44.1kHz
- Controlla clock I2S
- Prova sample rate diverso
```

### Rumore di Fondo
```
CAUSA: Alimentazione instabile o ground loop
SOLUZIONE:
- Migliora alimentazione
- Aggiungi condensatori filtro
- Separa ground digitale da analogico
```

## Debug Avanzato

### Abilita Logging Dettagliato
```cpp
#include <logger.h>

void setup() {
    set_log_level(LogLevel::DEBUG);
}
```

### Monitora Risorse di Sistema
```cpp
void loop() {
    static uint32_t last = 0;
    if (millis() - last > 5000) {
        LOG_INFO("Heap: %u, PSRAM: %u",
            heap_caps_get_free_size(MALLOC_CAP_8BIT),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        last = millis();
    }
    player.tick_housekeeping();
}
```

### Test Componenti Isolati
```cpp
// Test solo codec
codec.begin();
codec.setTone(1000); // Suono test
delay(1000);
codec.setTone(0);

// Test solo WiFi
WiFi.begin("SSID", "PASS");
while (!WiFi.isConnected()) delay(500);
LOG_INFO("IP: %s", WiFi.localIP().toString().c_str());

// Test solo SD
auto& sd = SdCardDriver::getInstance();
if (sd.begin()) {
    LOG_INFO("SD OK: %llu MB free", (sd.totalBytes() - sd.usedBytes()) / 1024 / 1024);
}
```

## Contatti Supporto

Se i problemi persistono:
1. Raccogli log completi con `DEBUG` level
2. Includi configurazione hardware completa
3. Descrivi comportamento atteso vs attuale
4. Apri issue su GitHub con informazioni dettagliate

## Checklist Risoluzione Rapida

- [ ] WiFi connesso prima di HTTP streaming?
- [ ] Filesystem inizializzato (LittleFS.begin())?
- [ ] SD card montata se usi SD storage?
- [ ] Codec inizializzato prima di AudioPlayer?
- [ ] `tick_housekeeping()` chiamato regolarmente?
- [ ] Volume > 0?
- [ ] URL stream valido e raggiungibile?
- [ ] Memoria sufficiente (heap > 100KB libero)?
- [ ] PSRAM abilitato se usi PSRAM mode?
