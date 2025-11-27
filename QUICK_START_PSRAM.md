# Quick Start: PSRAM-only Timeshift Mode

## TL;DR

Vuoi usare **solo PSRAM** (veloce, zero usura SD) invece della SD card per il timeshift?

### 3 Passi:

1. Connettiti al seriale e premi **`Z`**
2. Premi **`R`** per avviare il radio
3. Done! ✅

```
> Z
[INFO] Preferred timeshift storage mode set to: PSRAM_ONLY (fast, ~2min buffer)

> R
[INFO] Starting timeshift in PSRAM mode
[INFO] PSRAM pool allocated: 2048 KB (16 chunks x 128 KB)
[INFO] Timeshift radio playback started successfully!
```

## Cosa cambia?

### PSRAM Mode (comando `Z`)
- ⚡ **Veloce** - latenza ~0ms vs ~10ms SD
- 💾 **Nessuna usura SD** - tutto in RAM
- ⏱️ **Buffer limitato** - ~2 minuti @ 128kbps
- 🔄 **Circolare** - chunk vecchi sovrascritti automaticamente

### SD Mode (comando `C`, default)
- 📦 **Buffer illimitato** - limitato solo da spazio SD
- 🐢 **Più lento** - latenza ~10ms
- 💿 **Usa SD card** - scritture continue

## Comandi

| Comando | Azione |
|---------|--------|
| `W` | Mostra modalità preferita |
| `Z` | Imposta PSRAM mode |
| `C` | Imposta SD mode |
| `R` | Avvia radio (usa modalità impostata) |
| `Q` | Ferma radio |

## Workflow tipico

### Provare PSRAM mode
```
Z → R → [ascolta radio]
```

### Tornare a SD mode
```
Q → C → R
```

### Verificare modalità attiva
```
W
```

## Quando usare PSRAM?

✅ **Usa PSRAM quando:**
- Ascolti radio in diretta (non serve buffer lungo)
- Vuoi performance massime
- Vuoi preservare la SD card

❌ **Usa SD quando:**
- Vuoi fare pause lunghe (>2 minuti)
- Vuoi tornare indietro molto nel tempo
- Non hai PSRAM disponibile

## Note importanti

⚠️ **La modalità deve essere impostata PRIMA di avviare il radio**
- Non puoi cambiare modalità mentre lo stream è attivo
- Per cambiare: ferma con `Q`, imposta nuova modalità, riavvia con `R`

📊 **Memoria utilizzata:**
- PSRAM mode: 2MB PSRAM + 384KB RAM normale
- SD mode: 0KB PSRAM + 384KB RAM normale

## Logs di esempio

### Avvio in PSRAM mode
```
[INFO]  Starting timeshift in PSRAM mode
[INFO]  Timeshift mode: PSRAM_ONLY (16 chunks, 2048 KB total)
[INFO]  PSRAM pool allocated: 2048 KB (16 chunks x 128 KB)
[DEBUG] Wrote chunk 0: 124 KB to PSRAM (pool index 0)
[INFO]  First chunk ready! Starting playback...
[INFO]  Timeshift radio playback started successfully!
```

### Avvio in SD mode
```
[INFO]  Starting timeshift in SD mode
[INFO]  Timeshift mode: SD_CARD
[INFO]  Timeshift directory cleaned
[DEBUG] Wrote chunk 0: 124 KB to /timeshift/pending_0.bin
[INFO]  Chunk 0 promoted to READY (124 KB, offset 0-127663, 7993 ms)
[INFO]  First chunk ready! Starting playback...
[INFO]  Timeshift radio playback started successfully!
```

## Troubleshooting

### "Failed to allocate PSRAM pool"
- Il tuo ESP32 non ha PSRAM o è già tutta occupata
- Soluzione: usa SD mode con comando `C`

### Crash quando premo Z durante playback
- Non dovresti cambiare modalità durante playback
- Soluzione: ferma prima con `Q`, poi usa `Z` o `C`, poi riavvia con `R`

## Per sviluppatori

### Codice minimo
```cpp
#include "timeshift_manager.h"

auto* ts = new TimeshiftManager();
ts->setStorageMode(StorageMode::PSRAM_ONLY);  // O SD_CARD
ts->open("http://stream.radioparadise.com/mp3-128");
ts->start();
```

### Configurazione chunk pool
Modifica in [timeshift_manager.h](src/timeshift_manager.h#L60):
```cpp
static const size_t MAX_PSRAM_CHUNKS = 16;  // 16 chunks = 2MB
                                              // Aumenta per buffer più lungo
```

---

**Documentazione completa:** [TIMESHIFT_STORAGE_MODE.md](TIMESHIFT_STORAGE_MODE.md)
