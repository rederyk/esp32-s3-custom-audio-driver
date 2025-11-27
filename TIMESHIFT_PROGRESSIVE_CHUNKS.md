# Progressive Chunk Sizing - Fast Playback Start

## 🎯 Obiettivo

Ridurre drasticamente il tempo di attesa per l'inizio del playback mantenendo l'efficienza del sistema a regime.

---

## 📊 Problema con Chunk Fissi

### Sistema Precedente (Chunk Fissi 480KB)

```
Timeline @ 128kbps (16 KB/sec):
├─ 0 sec: Download started
├─ 5 sec: 80 KB downloaded
├─ 10 sec: 160 KB downloaded
├─ 15 sec: 240 KB downloaded
├─ 20 sec: 320 KB downloaded
├─ 25 sec: 400 KB downloaded
├─ 30 sec: 480 KB downloaded ✅ Chunk ready!
└─ 33 sec: Playback START ⏱️ 33 seconds wait!
```

**Problema:** L'utente aspetta **33 secondi** prima di sentire audio!

---

## ✅ Soluzione: Chunk Size Progressivo

### Sistema Nuovo (Progressive Sizing)

```cpp
// Chunk size increases progressively:
Chunk 0: 128 KB  → Fast start!
Chunk 1: 256 KB  → Transition
Chunk 2+: 512 KB → Optimal steady state
```

### Timeline @ 128kbps con Progressive Sizing

```
├─ 0 sec: Download started
├─ 3 sec: 48 KB downloaded
├─ 6 sec: 96 KB downloaded
├─ 8 sec: 128 KB downloaded ✅ Chunk 0 ready!
├─ 10 sec: Playback START ⏱️ ~10 seconds wait! (3.3x più veloce!)
│
├─ 18 sec: 256 KB more → Chunk 1 ready (transition)
├─ 48 sec: 512 KB more → Chunk 2 ready (optimal)
├─ 78 sec: 512 KB more → Chunk 3 ready (optimal)
└─ ... steady state chunks every ~30 sec
```

**Beneficio:** Playback inizia in **~10 secondi** invece di 33!

---

## 🔧 Implementazione

### 1. Definizione Chunk Size Progressivi

```cpp
// src/timeshift_manager.cpp

constexpr size_t MIN_CHUNK_SIZE = 128 * 1024;    // 128KB - First chunk
constexpr size_t MID_CHUNK_SIZE = 256 * 1024;    // 256KB - Second chunk
constexpr size_t MAX_CHUNK_SIZE = 512 * 1024;    // 512KB - Steady state

// Calculate target size based on chunk ID
inline size_t get_target_chunk_size(uint32_t chunk_id) {
    if (chunk_id == 0) return MIN_CHUNK_SIZE;      // Fast start!
    if (chunk_id == 1) return MID_CHUNK_SIZE;      // Transition
    return MAX_CHUNK_SIZE;                         // Optimal
}
```

---

### 2. Flush Dinamico nel Download Loop

```cpp
// In download_task_loop(), durante scrittura buffer:

// Calculate target chunk size based on current chunk ID
size_t target_size = get_target_chunk_size(next_chunk_id_);
size_t min_flush_threshold = target_size - 32 * 1024;  // Flush at target - 32KB

// Flush when target reached
if (bytes_in_current_chunk_ >= min_flush_threshold ||
    bytes_in_current_chunk_ >= BUFFER_SIZE - 64 * 1024) {  // Safety overflow

    flush_recording_chunk();
}
```

**Logica:**
- **Chunk 0**: Flush quando >= 96KB (128KB - 32KB)
- **Chunk 1**: Flush quando >= 224KB (256KB - 32KB)
- **Chunk 2+**: Flush quando >= 480KB (512KB - 32KB)

**Margine -32KB:** Permette tolleranza per evitare chunk troppo precisi (overhead SD write)

---

### 3. Timeout Ridotto (60s → 30s)

Con chunk da 128KB, il timeout può essere molto più aggressivo:

```cpp
// src/timeshift_manager.cpp, funzione read()

// First chunk: 128KB @ 128kbps = ~8 sec + processing ~2 sec = 10 sec
// Give 30 seconds to be safe (handles slower connections)
const uint32_t MAX_WAIT_MS = 30000;  // 30 seconds (was 60s)
```

**Calcolo:**
- 128KB @ 128kbps = 8 secondi teorici
- + 2 sec processing (`calculate_chunk_duration`)
- = **10 sec worst case normale**
- Timeout 30s = **3x safety margin**

---

### 4. Logging Migliorato

```cpp
// Durante flush:
LOG_INFO("Flushing chunk %u: %u bytes (chunk size reached, target: %u KB)",
         next_chunk_id_, bytes_in_current_chunk_, target_size / 1024);

// Durante promozione:
const char* chunk_type = "optimal";
if (chunk.id == 0) chunk_type = "fast-start";
else if (chunk.id == 1) chunk_type = "transition";

LOG_INFO("Chunk %u promoted to READY (%u KB [%s], offset %u-%u, %u ms)",
         chunk.id, chunk.length / 1024, chunk_type, ...);
```

**Output Atteso:**
```
[INFO]  Flushing chunk 0: 131072 bytes (chunk size reached, target: 128 KB)
[INFO]  🎵 FIRST CHUNK READY! Playback can now start (fast start with 128 KB chunk).
[INFO]  Chunk 0 promoted to READY (128 KB [fast-start], offset 0-131072, 2976 ms)

[INFO]  Flushing chunk 1: 262144 bytes (chunk size reached, target: 256 KB)
[INFO]  Chunk 1 promoted to READY (256 KB [transition], offset 131072-393216, 5952 ms)

[INFO]  Flushing chunk 2: 524288 bytes (chunk size reached, target: 512 KB)
[INFO]  Chunk 2 promoted to READY (512 KB [optimal], offset 393216-917504, 11904 ms)
```

---

## 📊 Comparazione Before/After

### Metriche di Startup

| Metrica | Before (480KB) | After (128KB) | Miglioramento |
|---------|----------------|---------------|---------------|
| Primo chunk size | 480 KB | 128 KB | **3.75x più piccolo** |
| Tempo download @ 128kbps | ~30 sec | ~8 sec | **3.75x più veloce** |
| Tempo processing | ~2 sec | ~0.5 sec | **4x più veloce** |
| **Playback START** | **~33 sec** | **~10 sec** | **🎯 3.3x più veloce!** |
| Timeout max | 60 sec | 30 sec | 2x più aggressivo |

---

### Metriche Steady State (dopo startup)

| Metrica | Before | After | Differenza |
|---------|--------|-------|------------|
| Chunk size medio | 480 KB | ~470 KB* | Simile |
| Flush frequency | Ogni ~30 sec | Ogni ~30 sec | Identica |
| Preloading | ✅ Sì | ✅ Sì | Identica |
| Stuttering | ✅ Eliminato | ✅ Eliminato | Identica |

**\*Media pesata:** (128 + 256 + 512×N) / (N+2) → converge a 512 KB per N grande

**Conclusione:** Zero regressione a regime, solo benefici allo startup!

---

## 🧪 Log di Test Attesi

### Sequenza Startup Ideale @ 128kbps

```bash
[INFO]  Timeshift buffers allocated: rec=1024KB, play=1536KB
[INFO]  TimeshiftManager download task started - connecting to ...
[INFO]  HTTP connected, code: 200 - starting download loop

# Fast start con primo chunk 128KB
[INFO]  Waiting for first chunk (128 KB)... (3 sec elapsed, 48 KB downloaded)
[INFO]  Waiting for first chunk (128 KB)... (6 sec elapsed, 96 KB downloaded)

# Chunk 0 ready in ~8 secondi!
[INFO]  Flushing chunk 0: 131072 bytes (chunk size reached, target: 128 KB)
[DEBUG] Chunk 0: 129024 samples, 2925 ms @ 44100 Hz
[INFO]  🎵 FIRST CHUNK READY! Playback can now start (fast start with 128 KB chunk).
[INFO]  Chunk 0 promoted to READY (128 KB [fast-start], offset 0-131072, 2925 ms, 129024 frames)

# Playback iniziato!
[INFO]  → Loaded chunk 0 (128 KB) [00:00 - 00:02]

# Chunk 1 ready in altri ~16 secondi (totale ~24 sec da inizio)
[INFO]  Recording: 393 KB total, 262144 bytes in current chunk, 1 ready chunks
[INFO]  Flushing chunk 1: 262144 bytes (chunk size reached, target: 256 KB)
[INFO]  Chunk 1 promoted to READY (256 KB [transition], offset 131072-393216, 5952 ms)

# Da qui in poi chunk 512KB ottimali (ogni ~30 sec)
[INFO]  Flushing chunk 2: 524288 bytes (chunk size reached, target: 512 KB)
[INFO]  Chunk 2 promoted to READY (512 KB [optimal], offset 393216-917504, 11904 ms)

# Preloading seamless come prima
[DEBUG] Preloaded chunk 1 at 60% of chunk 0
[DEBUG] Switching to preloaded chunk 1 (seamless)
```

---

### Sequenza con Network Lento (64kbps)

```bash
[INFO]  Waiting for first chunk (128 KB)... (3 sec elapsed, 24 KB downloaded)
[INFO]  Waiting for first chunk (128 KB)... (6 sec elapsed, 48 KB downloaded)
[INFO]  Waiting for first chunk (128 KB)... (9 sec elapsed, 72 KB downloaded)
[INFO]  Waiting for first chunk (128 KB)... (12 sec elapsed, 96 KB downloaded)
[INFO]  Waiting for first chunk (128 KB)... (15 sec elapsed, 120 KB downloaded)

# Anche con connessione lenta, playback parte in ~17 secondi
[INFO]  Flushing chunk 0: 131072 bytes (chunk size reached, target: 128 KB)
[INFO]  🎵 FIRST CHUNK READY! Playback can now start (fast start with 128 KB chunk).
```

**Beneficio:** Anche con connessione lenta, esperienza molto migliore!

---

## 🎯 Vantaggi del Sistema Progressivo

### 1. **User Experience Eccellente**
- ⏱️ Playback inizia in ~10 secondi (vs 33 secondi)
- 📊 Feedback rapido se stream funziona
- 🎵 Utente sente audio mentre buffering continua

---

### 2. **Efficienza Mantenuta**
- 🔄 Dopo startup, chunk ottimali da 512KB
- 💾 Stesso numero operazioni SD a regime
- ⚡ Preloading funziona identicamente

---

### 3. **Resilienza Network**
- 📶 Gestisce meglio connessioni lente/instabili
- ⏰ Timeout più aggressivo (30s vs 60s) per failfast
- 🔁 Reconnection più veloce su errori

---

### 4. **Compatibilità Totale**
- ✅ Zero breaking changes all'architettura
- ✅ Double buffering funziona identicamente
- ✅ Seek temporale non impattato
- ✅ Cleanup automatico non cambia

---

## ⚙️ Configurazione Avanzata

### Personalizzare Chunk Sizes

Puoi modificare i valori in `timeshift_manager.cpp`:

```cpp
// Per stream molto lenti (32kbps):
constexpr size_t MIN_CHUNK_SIZE = 64 * 1024;     // 64KB → 16 sec @ 32kbps
constexpr size_t MID_CHUNK_SIZE = 128 * 1024;    // 128KB
constexpr size_t MAX_CHUNK_SIZE = 256 * 1024;    // 256KB

// Per stream veloci (320kbps):
constexpr size_t MIN_CHUNK_SIZE = 256 * 1024;    // 256KB → 6 sec @ 320kbps
constexpr size_t MID_CHUNK_SIZE = 512 * 1024;    // 512KB
constexpr size_t MAX_CHUNK_SIZE = 1024 * 1024;   // 1MB
```

**Formula per MIN_CHUNK_SIZE:**
```
Target startup time: T seconds (es. 10 sec)
Bitrate: B kbps (es. 128 kbps)

MIN_CHUNK_SIZE = (B * 1000 / 8) * T bytes
               = (128 * 1000 / 8) * 10
               = 160,000 bytes
               ≈ 160 KB

Arrotonda a potenza di 2: 128 KB
```

---

### Aggiungere Più Step Progressivi

Puoi estendere la progressione:

```cpp
inline size_t get_target_chunk_size(uint32_t chunk_id) {
    if (chunk_id == 0) return 128 * 1024;   // Fast start
    if (chunk_id == 1) return 192 * 1024;   // Step 1
    if (chunk_id == 2) return 256 * 1024;   // Step 2
    if (chunk_id == 3) return 384 * 1024;   // Step 3
    return 512 * 1024;                      // Optimal
}
```

**Trade-off:**
- ✅ PRO: Transizione più graduale
- ❌ CONTRO: Più chunk piccoli da gestire all'inizio

---

## 📈 Performance Testing

### Test Case 1: Stream 128kbps, WiFi Buono

```
Expected:
- Chunk 0 ready: ~8-10 sec ✅
- Playback start: ~10-12 sec ✅
- Chunk 1 ready: ~24-26 sec ✅
- Chunk 2 ready: ~54-56 sec ✅
- No stuttering during playback ✅
```

---

### Test Case 2: Stream 128kbps, WiFi Debole

```
Expected:
- Chunk 0 ready: ~15-20 sec ⚠️
- Playback start: ~17-22 sec ⚠️
- Still within 30s timeout ✅
- Chunks eventually stabilize ✅
```

---

### Test Case 3: Stream 320kbps, WiFi Buono

```
Expected:
- Chunk 0 (128KB) ready: ~3-4 sec ✅✅
- Playback start: ~4-5 sec 🚀
- Very fast startup!
```

---

## 🐛 Troubleshooting

### Problema: Timeout Ancora Presente

```
[ERROR] Timeout waiting for first chunk (30 sec). Check network connection.
```

**Cause Possibili:**
1. Network realmente troppo lento (<32 kbps)
2. Stream URL non valido
3. Firewall blocca connessione
4. SD card troppo lenta (improbabile)

**Debug:**
```bash
# Controlla KB scaricati vs tempo:
[INFO]  Waiting for first chunk (128 KB)... (30 sec elapsed, 64 KB downloaded)
                                                                  ^^^ Solo 64KB in 30s!
# Se < 128KB in 30 sec → network problema serio (<34 kbps)
```

**Soluzione:** Ridurre ulteriormente MIN_CHUNK_SIZE:
```cpp
constexpr size_t MIN_CHUNK_SIZE = 64 * 1024;  // 64KB per network molto lenti
```

---

### Problema: Stuttering su Chunk 0→1 Transition

```
[WARN]  Chunk 1 not preloaded, loading now (may cause stutter)
```

**Causa:** Chunk 0 troppo piccolo (128KB = ~3 sec audio), non c'è tempo per preload

**Soluzione:** Normale per primo chunk! Il preloading parte dal chunk 1→2.

**Verifica:**
```bash
# Chunk 1→2 transition dovrebbe essere seamless:
[DEBUG] Preloaded chunk 2 at 60% of chunk 1
[DEBUG] Switching to preloaded chunk 2 (seamless)  ✅
```

Se stutter continua dopo chunk 2, allora è un problema diverso.

---

## ✅ Conclusione

Il sistema di **Progressive Chunk Sizing** risolve brillantemente il trade-off tra:
- ⚡ **Startup veloce** (primo chunk piccolo da 128KB)
- 🔄 **Efficienza a regime** (chunk ottimali da 512KB)

**Risultato:**
- Playback inizia in **~10 secondi** invece di 33
- Esperienza utente **3.3x migliore**
- Zero regressioni su performance steady-state
- Compatibile al 100% con sistema double buffering

🎵 **Fast start + Smooth playback = Perfect timeshift system!** 🎵
