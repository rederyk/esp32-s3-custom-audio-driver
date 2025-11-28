# Timeshift Storage Mode - Guida all'uso

Il `TimeshiftManager` supporta due modalità di storage:

## Modalità disponibili

### 1. `StorageMode::SD_CARD` (Default)
- **Vantaggi:**
  - Basso utilizzo di memoria PSRAM
  - Può salvare quantità illimitate di dati (limitato solo dallo spazio su SD)
  - Nessun limite di timeshift window oltre i 2MB configurati

- **Svantaggi:**
  - Latenza di lettura/scrittura dalla SD card
  - Usura della SD card con uso prolungato
  - Richiede SD card funzionante

### 2. `StorageMode::PSRAM_ONLY`
- **Vantaggi:**
  - **Latenza minima** - accesso diretto alla RAM
  - **Nessuna usura** - nessuna scrittura su SD
  - **Performance massime** - memcpy invece di file I/O
  - Ideale per streaming continuo senza SD

- **Svantaggi:**
  - Usa PSRAM adattivo basato su bitrate (chunk da 16KB-512KB)
  - Timeshift window limitato a ~2MB (circa 2 minuti @ 128kbps)
  - I chunk più vecchi vengono sovrascritti automaticamente (buffer circolare)

## Come selezionare la modalità

### ⚠️ IMPORTANTE: Imposta la modalità PRIMA di avviare il radio

La modalità storage deve essere selezionata **prima** di avviare lo streaming con il comando `r`.
Non è possibile cambiare modalità mentre lo stream è attivo.

### Workflow corretto:

```
1. [Opzionale] Premi 'W' per vedere la modalità preferita corrente
2. Premi 'Z' per PSRAM o 'C' per SD card
3. Premi 'R' per avviare il radio (userà la modalità selezionata)
```

### Comandi disponibili

Una volta connesso via seriale:

- **`W`** - Mostra la modalità preferita corrente
- **`Z`** - Imposta preferenza PSRAM mode (veloce, ~2min buffer)
- **`C`** - Imposta preferenza SD Card mode (lento, buffer illimitato)
- **`R`** - Avvia radio con la modalità selezionata

### Esempio sessione seriale

```
> W
[INFO]  Preferred timeshift storage mode: SD_CARD
[INFO]    - Slower access, unlimited buffer, uses SD card
[INFO]  Use 'Z' for PSRAM or 'C' for SD, then start radio with 'r'

> Z
[INFO]  Preferred timeshift storage mode set to: PSRAM_ONLY (fast, ~2min buffer)
[INFO]  This will be used next time you start radio with 'r' command

> R
[INFO]  Starting timeshift in PSRAM mode
[INFO]  Timeshift mode: PSRAM_ONLY (16 chunks, 2048 KB total)
[INFO]  PSRAM pool allocated: 2048 KB (16 chunks x 128 KB)
...
[INFO]  Timeshift radio playback started successfully!
```

## Cambiare modalità

Per cambiare modalità:

1. **Ferma** il playback corrente con `q`
2. **Imposta** la nuova modalità con `Z` o `C`
3. **Riavvia** il radio con `R`

```
> q                    # Stop playback
> C                    # Set SD card mode
> R                    # Start radio in SD mode
```

## Esempio codice (per uso programmatico)

### Impostare modalità prima di avviare

```cpp
#include "timeshift_manager.h"

void setup() {
    auto* ts = new TimeshiftManager();

    // Imposta modalità PRIMA di open()
    ts->setStorageMode(StorageMode::PSRAM_ONLY);

    // Ora apri lo stream - userà PSRAM
    ts->open("http://stream.radioparadise.com/mp3-128");
    ts->start();
}
```

### Riavviare con modalità diversa

```cpp
void restartWithNewMode(TimeshiftManager* ts, StorageMode new_mode) {
    String url = String(ts->uri());

    // 1. Ferma e chiudi
    ts->stop();
    ts->close();

    // 2. Cambia modalità
    ts->setStorageMode(new_mode);

    // 3. Riapri con nuova modalità
    ts->open(url.c_str());
    ts->start();
}
```

## Monitoraggio

I log ti diranno quale modalità è attiva:

### SD Mode
```
[INFO]  Timeshift mode: SD_CARD
[INFO]  Timeshift directory cleaned
[DEBUG] Wrote chunk 0: 124 KB to /timeshift/pending_0.bin
```

### PSRAM Mode
```
[INFO]  Timeshift mode: PSRAM_ONLY (16 chunks, 2048 KB total)
[INFO]  PSRAM pool allocated: 2048 KB (16 chunks x 128 KB)
[DEBUG] Wrote chunk 0: 124 KB to PSRAM (pool index 0)
```

## Limiti e considerazioni

### PSRAM Mode
- **Max timeshift window:** ~2MB (configurabile cambiando `MAX_PSRAM_CHUNKS` in timeshift_manager.h)
- **Durata disponibile:** circa 2 minuti @ 128kbps, 1 minuto @ 256kbps
- **Chunk circolari:** i chunk più vecchi vengono automaticamente sovrascritti quando il pool è pieno
- **Memoria richiesta:** 2MB di PSRAM (verificare disponibilità con `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`)

### SD Card Mode
- **Max timeshift window:** configurato a 2MB nel codice, ma può essere aumentato
- **Nessun limite pratico** se c'è spazio su SD
- **Cleanup automatico** dei chunk più vecchi oltre il window configurato

## Uso consigliato

- **PSRAM mode:** Ideale per streaming radio continuo dove non serve timeshift lungo
- **SD mode:** Quando serve timeshift > 2 minuti o quando vuoi preservare la PSRAM per altro

## Perché non si può cambiare a runtime?

Il cambio modalità a runtime richiederebbe di:
1. Fermare il download task
2. Fermare il playback task
3. Liberare memoria (SD files o PSRAM pool)
4. Riallocare memoria nella nuova modalità
5. Riavviare tutti i task

Questo è tecnicamente complesso e può causare crash se il player sta ancora accedendo ai dati.
**È più sicuro** impostare la modalità prima di avviare lo stream.

## Technical Details

- Il cambio modalità richiede `stop()` → `close()` → `setStorageMode()` → `open()` → `start()`
- Il buffer di playback (256KB) è sempre in RAM normale (non PSRAM)
- Il recording buffer (128KB) è sempre in RAM normale
- Solo i chunk completati vanno in PSRAM/SD
- Pre-loading seamless funziona in entrambe le modalità
