# Timeshift UX Improvements - User Manual

## 🎯 Problema Risolto

**Prima**: Comando `r` confuso, richiedeva 3 step separati (r, l, p) e il decoder partiva prima che i chunk fossero pronti.

**Ora**: Comando `r` fa **tutto automaticamente** in un solo step!

---

## 🚀 Nuovo Comando Radio Streaming

### Comando Semplificato

```
r      # Avvia radio streaming con timeshift (tutto in uno!)
```

### Cosa Fa Automaticamente

1. ✅ **Stop playback corrente** (se attivo)
2. ✅ **Crea TimeshiftManager**
3. ✅ **Apre stream HTTP** (http://stream.radioparadise.com/mp3-128)
4. ✅ **Avvia download task** (registrazione in background)
5. ✅ **Aspetta primo chunk READY** (max 10 secondi)
6. ✅ **Arma la sorgente** (arm_source)
7. ✅ **Avvia playback** automaticamente!

### Output Atteso

```
> r

[INFO]  Stopping current playback before starting timeshift...
[INFO]  Timeshift buffers allocated: 2x128KB
[INFO]  TimeshiftManager download task created successfully
[INFO]  Timeshift download started, waiting for first chunk...
[INFO]  Waiting for chunks... (128 KB downloaded)
[INFO]  Waiting for chunks... (256 KB downloaded)
[INFO]  Waiting for chunks... (384 KB downloaded)
[INFO]  Waiting for chunks... (512 KB downloaded)
[INFO]  Chunk 0 promoted to READY (512 KB, offset 0-524288)
[INFO]  First chunk ready! Starting playback...
[INFO]  Loaded chunk 0 to playback buffer (512 KB)
[INFO]  Timeshift radio playback started successfully!
```

---

## 📋 Altri Comandi Semplificati

### Playback File Locali

**Prima**:
```
t      # Seleziona test file
l      # Arma
p      # Play
```

**Ora**:
```
t      # Riproduci test file (fa tutto!)
s      # Riproduci sample file (fa tutto!)
```

Entrambi ora fanno automaticamente: stop → select → arm → play

---

## 🎮 Comandi Disponibili

### Playback
```
r      - Avvia radio streaming con timeshift (tutto in uno!)
t      - Riproduci test file (/sample-rich.mp3)
s      - Riproduci sample file (/audioontag.mp3)
p      - Play/Pause toggle
q      - Stop playback
```

### Controllo
```
v##    - Volume (es. v75 = 75%)
i      - Stato player (mostra chunk, buffer, bitrate)
```

### File System
```
d [path]  - Lista file (es. 'd /' o 'd /sd/')
f<path>   - Seleziona file custom (es. f/song.mp3)
x         - Stato SD card (spazio, tipo, errori)
```

### Debug
```
m      - Memory stats (heap usage)
h      - Mostra help
```

---

## 🔍 Monitorare il Timeshift

### Comando `i` - Player Status

```
> i

[INFO]  --- Player Status ---
[INFO]  State: PLAYING
[INFO]  Sample Rate: 44100 Hz
[INFO]  Buffered: 512 KB (ready chunks available)
[INFO]  Downloaded: 1024 KB total
[INFO]  Ready Chunks: 2
[INFO]  Current Offset: 245760 bytes
```

### Logs Chiave

**Chunk Promotion**:
```
[INFO]  Chunk 0 promoted to READY (512 KB, offset 0-524288)
```
→ Ogni ~4 secondi un nuovo chunk di 512KB viene completato

**Chunk Loading**:
```
[DEBUG] Loaded chunk 1 to playback buffer (512 KB)
```
→ Quando passi da un chunk all'altro durante riproduzione

**Recording Progress**:
```
[INFO]  Recording: 1024 KB total, 128 bytes in current chunk, 2 ready chunks
```
→ Ogni 5 secondi mostra stato della registrazione

---

## 🧪 Test Procedure

### Test 1: Avvio Base

```
> r         # Avvia radio

Atteso:
- Download parte
- Dopo ~4-5 sec, primo chunk READY
- Playback parte automaticamente
- Audio fluido senza interruzioni
```

### Test 2: Pausa/Resume

```
> r         # Avvia radio
> p         # Pausa dopo 10 secondi
            # Attendi 5 secondi
> p         # Riprendi

Atteso:
- Pausa silenzia audio
- Recording continua in background
- Resume riprende esattamente dal punto di pausa
- Nessun glitch o salti
```

### Test 3: Stop/Restart

```
> r         # Avvia radio
> q         # Stop completo
> r         # Riavvia

Atteso:
- Stop pulito (cleanup chunk)
- Riavvio riparte da zero
- Nessun file residuo su SD
```

### Test 4: Cambio Sorgente

```
> r         # Avvia radio
> t         # Passa a test file

Atteso:
- Radio si stoppa automaticamente
- Test file parte immediatamente
- Nessun conflitto tra sorgenti
```

---

## ⚙️ Configurazione

### File: `src/main.cpp`

```cpp
// URL stream radio (modificabile)
static const char *kRadioStreamURL = "http://stream.radioparadise.com/mp3-128";

// Timeout attesa primo chunk (default 10 sec)
const uint32_t MAX_WAIT_MS = 10000;
```

### File: `src/timeshift_manager.cpp`

```cpp
// Dimensione chunk (default 512KB)
constexpr size_t CHUNK_SIZE = 512 * 1024;

// Finestra massima timeshift (default 512MB)
constexpr size_t MAX_TS_WINDOW = 1024 * 1024 * 512;

// Timeout riconnessione stream (default 30 sec)
const uint32_t STREAM_TIMEOUT = 30000;
```

---

## 🐛 Troubleshooting

### Problema: "Timeout waiting for ready chunks"

**Causa**: Connessione lenta o stream non disponibile

**Soluzione**:
1. Verifica WiFi: `WiFi connected! IP: 192.168.x.x`
2. Prova URL stream diverso
3. Aumenta `MAX_WAIT_MS` a 15000 (15 sec)

---

### Problema: "Failed to flush recording chunk"

**Causa**: SD card piena o danneggiata

**Soluzione**:
1. Verifica SD: `> x` (comando stato SD)
2. Controlla spazio libero
3. Prova `> d /sd/` per listare file
4. Rimuovi vecchi chunk manualmente se necessario

---

### Problema: Audio interrotto/glitchy

**Causa**: SD card lenta o chunk non pronti in tempo

**Soluzione**:
1. Usa SD card Class 10 o superiore
2. Riduci `CHUNK_SIZE` a 256KB (più chunk piccoli = latenza minore)
3. Aumenta `BUFFER_SIZE` a 256KB (più RAM cache)

---

## 📊 Performance Attese

### Latenza
- **Avvio playback**: 4-5 secondi (tempo per primo chunk 512KB)
- **Cambio chunk**: < 100ms (caricamento da SD)
- **Pausa/Resume**: Istantaneo

### Memoria
- **RAM usata**: ~256KB (2x buffer 128KB)
- **SD max**: 512MB (finestra scorrevole con cleanup automatico)

### Bitrate Stream
- **Radio Paradise 128k**: ~16 KB/sec
- **Chunk duration**: ~32 secondi per chunk 512KB
- **Buffer safety**: ~2-3 chunk sempre disponibili

---

## ✅ Checklist Pre-Test

Prima di testare il timeshift, verifica:

- [x] SD card inserita e montata
- [x] WiFi connesso (`WiFi connected! IP: x.x.x.x`)
- [x] Spazio libero su SD > 600MB
- [x] Heap libero > 8MB (`> m` per verificare)
- [x] Compilato senza errori

---

## 🎯 Prossimi Step

1. **Flash firmware**:
   ```bash
   pio run -t upload -t monitor
   ```

2. **Testa comando base**:
   ```
   > r
   ```

3. **Verifica logs**:
   - `Chunk X promoted to READY`
   - `Timeshift radio playback started successfully!`

4. **Testa pausa/resume**:
   ```
   > p (pausa)
   > p (resume)
   ```

5. **Monitora con**:
   ```
   > i (status ogni 10 sec)
   ```

Buon ascolto! 🎵
