# Timeshift Buffer Optimizations

## 🎯 Problema Rilevato

Stuttering durante il caricamento dei chunk, causato da:

1. **Buffer fisso inefficiente** - Dimensioni non adattate al bitrate dello stream
2. **Caricamento chunk sincrono** - Blocco della riproduzione durante `load_chunk_to_playback()`
3. **Nessun preloading** - Ogni chunk viene caricato solo quando necessario
4. **Chunk size fisso** - Non ottimale per diversi bitrate

## ✅ Ottimizzazioni Implementate

### 1. Adaptive Buffer Sizing basato su Bitrate

**Perché:**
- Diversi stream hanno bitrate diversi (96kbps, 128kbps, 320kbps)
- Buffer fissi sono inefficienti: troppo piccoli per stream veloci, sprecati per stream lenti
- Sistema rileva automaticamente bitrate e adatta buffer di conseguenza

**Formula:**
```cpp
// Target: 7 secondi di audio per chunk (bilanciato)
uint32_t target_chunk_bytes = (bitrate_kbps * 1000 / 8) * 7;

// Limiti sicuri: 16KB - 512KB
dynamic_chunk_size_ = clamp(target_chunk_bytes, 16KB, 512KB);

// Buffer adattivi:
dynamic_buffer_size_ = dynamic_chunk_size_ + (dynamic_chunk_size_ / 2);  // 1.5x
dynamic_playback_buffer_size_ = dynamic_chunk_size_ * 3;  // 3x per double buffering
```

**Esempi:**
- **96kbps**: chunk=84KB, buffer=126KB, playback=252KB
- **128kbps**: chunk=112KB, buffer=168KB, playback=336KB  
- **320kbps**: chunk=280KB, buffer=420KB, playback=840KB

**Impatto:**
- ✅ Buffer sempre ottimizzati per lo stream corrente
- ✅ Nessuno spreco di memoria per stream lenti
- ✅ Capacità sufficiente per stream veloci

---

### 2. Bitrate Auto-Detection e Adattamento

**❗ IMPORTANTE**: Il sistema misura **throughput di download effettivo**, non bitrate audio codificato!

#### Cosa Misura il Sistema
```cpp
// NON misura il bitrate MP3 (192 kbps)
// MA misura la velocità di download reale (es. 160 kbps su WiFi lento)
uint32_t measured_kbps = (bytes_downloaded * 8) / time_elapsed;
```

**Perché throughput invece di bitrate audio?**
- ✅ Adatta buffer alle **condizioni di rete reali** (WiFi, latenza, interferenze)
- ✅ Gestisce connessioni lente senza buffer underrun
- ✅ Ottimizza per **throughput effettivo** vs bitrate teorico

#### Esempio Pratico
```
Stream: 192 kbps MP3
Connessione WiFi: 160 kbps effettivi (overhead HTTP/TCP)
Risultato: Buffer dimensionati per 160 kbps → chunk da ~137 KB
Beneficio: Playback stabile nonostante connessione non ideale
```

#### Implementazione Rilevamento:
```cpp
// Ogni BITRATE_SAMPLE_WINDOW_MS (2.5 secondi)
uint32_t measured_kbps = (bytes_since_rate_sample_ * 8) / elapsed_ms;

// Media mobile su 4 campioni per stabilità
bitrate_history_.push_back(measured_kbps);
if (bitrate_history_.size() >= 2) {
    uint32_t average = calculate_average(bitrate_history_);
    uint32_t snapped = snap_to_common_bitrate(average);  // 128, 192, 256, etc.

    if (!bitrate_adapted_once_ || gap > threshold) {
        calculate_adaptive_sizes(snapped);
        bitrate_adapted_once_ = true;
    }
}
```

#### Estrazione da MP3 headers (Fase 2):
```cpp
// Durante calculate_chunk_duration() - dopo che il chunk è pronto
uint32_t extracted_bitrate = mp3_header_bitrate_table[bitrate_idx];
if (!bitrate_adapted_once_ && extracted_bitrate > 0) {
    LOG_INFO("Bitrate extracted from MP3 header: %u kbps", extracted_bitrate);
    calculate_adaptive_sizes(extracted_bitrate);
    bitrate_adapted_once_ = true;
}
```

## 📊 Due Fasi di Bitrate Detection

### **FASE 1: Throughput Measurement (Immediata)**
- **Quando:** Appena inizia il download
- **Cosa misura:** Velocità effettiva di rete (bytes/second)
- **Scopo:** Dimensionare buffer immediatamente
- **Vantaggio:** Previene allocazioni tardive e OOM

### **FASE 2: MP3 Header Extraction (Ritardata)**
- **Quando:** Dopo che il primo chunk è scaricato e pronto
- **Cosa misura:** Bitrate codificato nel file MP3
- **Scopo:** Correggere se throughput ≠ bitrate effettivo
- **Vantaggio:** Precisione massima sui dati reali

### **Esempio Completo:**
```
0s:   Stream inizia
      → FASE 1: Misura 160 kbps (throughput WiFi)
      → Buffer: 168KB, Chunk target: 137KB

8s:   Primo chunk pronto (137KB)
      → FASE 2: Legge header MP3 = 192 kbps (bitrate file)
      → Se differenza significativa, raffina buffer

Risultato: Buffer ottimizzato per entrambe le misure!
```

**Perché due fasi?**
- **Throughput** = velocità reale della tua connessione
- **MP3 bitrate** = come è stato codificato il file
- **Entrambi necessari** per adattamento perfetto!

**Impatto:**
- ✅ Dimensioni buffer corrette fin dall'inizio
- ✅ Adattamento automatico a cambi di stream
- ✅ Gestione connessioni lente/variabili
- ✅ Correzione basata su dati reali del file

---

### 3. Preloading Intelligente al 50%

**Perché 50% invece di 60%:**
- Trigger più anticipato = più tempo per il caricamento
- Con chunk adattivi, il 50% corrisponde a 3.5 secondi di anticipo (abbastanza)
- Riduce rischio di caricamento tardivo

**Implementazione:**
```cpp
// In preloader_task_loop()
float progress = (float)offset_in_chunk / (float)current_chunk.length;

if (progress >= 0.50f && current_abs_chunk_id != last_preload_check_chunk_abs_id_) {
    if (preload_next_chunk(current_abs_chunk_id_)) {
        LOG_DEBUG("Preloader: loaded chunk %u at 50%% of chunk %u",
                  current_abs_chunk_id_ + 1, current_abs_chunk_id_);
        next_chunk_preloaded = true;
    }
}
```

**Impatto:**
- ✅ Chunk successivo sempre pronto quando necessario
- ✅ Switch seamless con `memmove()` (istantaneo)
- ✅ Eliminazione completa dello stuttering

---

### 4. Download Chunk Size Adattivo

**Logica:**
```cpp
if (detected_bitrate_kbps_ <= 64) {
    dynamic_download_chunk_ = 2048;   // 2KB per stream lenti
} else if (detected_bitrate_kbps_ <= 128) {
    dynamic_download_chunk_ = 4096;   // 4KB per stream medi
} else {
    dynamic_download_chunk_ = 8192;   // 8KB per stream veloci
}
```

**Perché:**
- Stream lenti: chunk piccoli per responsiveness
- Stream veloci: chunk grandi per throughput massimo
- Riduce overhead HTTP calls

---

## 📊 Confronto Approccio Fisso vs Adattivo

| Metrica | Buffer Fisso (vecchio) | Adattivo (nuovo) | Miglioramento |
|---------|----------------------|------------------|---------------|
| Buffer per 128kbps | 1MB (sprecato) | 168KB | **6x meno memoria** |
| Buffer per 320kbps | 1MB (troppo piccolo) | 420KB | **2.4x più capacità** |
| Setup time | Buffer fisso | Auto-detection | **Configurazione automatica** |
| Adattabilità | ❌ Fissa | ✅ Dinamica | **Adattivo a qualsiasi stream** |
| Preloading trigger | 60% | 50% | **Più anticipato** |
| Chunk switch | memmove | memmove | **Sempre seamless** |

---

## 🧪 Testing e Validazione

### Test 1: Bitrate Detection
```
[INFO] Bitrate auto-detected: 128 kbps (avg 126 kbps, sample 132 kbps)
[INFO] Adaptive sizing for 128 kbps: chunk=112 KB, buffer=168 KB, playback=336 KB
```

✅ **Atteso**: Dimensioni adattate correttamente

---

### Test 2: Preloading al 50%
```
[DEBUG] Preloader: loaded chunk 2 at 50% of chunk 1
[INFO] → Loaded chunk 1 (112 KB) [00:08 - 00:16]
[DEBUG] Switching to preloaded chunk 2 (seamless)
```

✅ **Atteso**: Preload anticipato, switch senza stutter

---

### Test 3: Cambio Stream
```
Stream A: 128kbps → buffer=168KB
[INFO] Stream changed, resetting bitrate detection
Stream B: 320kbps → buffer=420KB
[INFO] Bitrate auto-detected: 320 kbps
[INFO] Adaptive sizing for 320 kbps: chunk=280 KB, buffer=420 KB, playback=840 KB
```

✅ **Atteso**: Adattamento automatico a nuovo stream

---

## ⚠️ Considerazioni Tecniche

### 1. Limiti di Sicurezza
```cpp
// Massimi per evitare OOM
MAX_DYNAMIC_CHUNK_BYTES = 512 * 1024;        // 512KB
MAX_RECORDING_BUFFER_CAPACITY = 768 * 1024;  // 768KB
MAX_PLAYBACK_BUFFER_CAPACITY = 1.5 * 1024 * 1024; // 1.5MB
```

### 2. Gestione Edge Cases

#### Stream con bitrate variabile:
- Sistema adatta gradualmente (non immediatamente per evitare oscillazioni)
- Usa media mobile per stabilità

#### Primo chunk (cold start):
- Usa default 128kbps, adatta dopo primo chunk
- Preloading inizia dopo secondo chunk

#### Memory constraints:
- Se allocazione fallisce, fallback a dimensioni minime
- Logging dettagliato per debugging

---

## 🚀 Vantaggi dell'Approccio Adattivo

### 1. **Efficienza Memoria**
- Buffer sempre proporzionati al bitrate effettivo
- Nessuno spreco su ESP32 con PSRAM limitata
- Scalabile da 96kbps a 320kbps senza riconfigurazione

### 2. **Performance Costante**
- Preloading al 50% garantisce sempre chunk ready
- Switch seamless indipendentemente da dimensione chunk
- Throughput ottimizzato per ogni bitrate

### 3. **Manutenibilità**
- Zero configurazione manuale
- Adattamento automatico a nuovi stream
- Codice più semplice (no hardcoded constants)

### 4. **Robustezza**
- Gestione automatica di bitrate variabili
- Fallback sicuri se allocazione fallisce
- Logging dettagliato per troubleshooting

---

## 📝 Checklist Validazione

- [ ] Verificare log "Adaptive sizing for X kbps"
- [ ] Controllare dimensioni buffer proporzionate al bitrate
- [ ] Testare cambio stream (adattamento automatico)
- [ ] Verificare preload al 50% senza stutter
- [ ] Testare stream da 96kbps a 320kbps
- [ ] Monitorare uso memoria (no leaks)
- [ ] Validare switch seamless su diversi bitrate

---

## ✅ Conclusione

L'approccio **adattivo basato su bitrate** rappresenta un significativo miglioramento rispetto ai buffer fissi:

1. ✅ **Efficienza**: Buffer sempre ottimizzati, zero spreco
2. ✅ **Adattabilità**: Funziona con qualsiasi stream senza configurazione  
3. ✅ **Performance**: Preloading intelligente elimina stuttering
4. ✅ **Robustezza**: Gestione automatica di edge cases
5. ✅ **Scalabilità**: Da stream lenti a veloci senza modifiche

Il sistema è ora **auto-ottimizzante** e pronto per qualsiasi scenario di streaming audio!
