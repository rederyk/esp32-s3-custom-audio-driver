# 🚀 Roadmap Custom Audio Driver ESP32-S3 (MIT License)

## 🎯 Obiettivo Finale
Creare un player audio professionale completamente open-source MIT che sostituisca ESP32-audioI2S GPL-3.0, con prestazioni superiori e controllo totale.

## ✅ Stato Finale - Progetto Completato! 🎉
- [x] **Driver ES8311 funzionante** con MCLK da BCLK (evita problemi ESP32-S3)
- [x] **Decoder dr_mp3 integrato** per MP3 stereo fino a 44.1kHz
- [x] **Streaming da LittleFS** invece di SD card (più affidabile)
- [x] **Architettura producer-consumer** con FreeRTOS ring buffer (single ring configurabile PSRAM/DRAM)
- [x] **Ottimizzazione memoria** (PSRAM per decoder, DMA per I2S)
- [x] **Licenza Apache 2.0 completa** (nessuna dipendenza GPL)
- [x] **Documentazione aggiornata** (README e AUDIO_GUIDE riscritti)
- [x] **Sistema di logging strutturato** (livelli ERROR/WARN/INFO/DEBUG)
- [x] **Controlli seriali di base** (play/stop via Serial)
- [x] **Gestione errori migliorata**: retry esponenziale per ring buffer send/receive (5 tentativi)

## 🔧 Fase 1: Ottimizzazioni Core (1-2 settimane)

### 1.1 Miglioramento Error Handling & Robustness
- [x] **Gestione errori migliorata**: Aggiungere retry per fallback su ring buffer
- [x] **Logging strutturato**: Sistema di log con livelli (ERROR/WARN/INFO/DEBUG)
- [x] **Timeout handling**: Timeout configurabili per operazioni I/O (ringbuffer send/recv, stop sincronizzato)
- [x] **Recovery automatica**: Restart decoder su error critici (underrun/I2S/write/init) con limiti tentativi
- [x] **Stop sicuro**: EventGroup per shutdown task audio/file e cleanup deterministico
- [x] **Fix crash su stop**: Gestione sicura task handle (no double delete, handle nulled dai task)
- [x] **Stima durata realistica**: Parser header MP3 per stimare bitrate reale (niente più 146s su file da 59s)

### 1.2 Ottimizzazione Performance
- [x] **callback ring buffer bulk read**: Lettura multi-item invece di single-item (con gestione leftover e backpressure sul producer)
- [x] **Decoder s16 e I2S chunked**: Decodifica diretta a int16 + write a chunk fissi (no float convert)
- [x] **Configurazione ring PSRAM/DRAM**: Build flag `AUDIO_RING_USE_DRAM` per ring ridotto e veloce (16/32KB) o PSRAM ampio (128KB)
- [x] **Buffer sizing dinamico**: Calcolo dimensioni ottimali basato su sample rate
- [x] **Task priorities ottimizzate**: Audio task con prio 6 su core 1, file producer prio 4 su core 0 (fallback no-affinity se core singolo)
- [x] **I2S DMA tuning**: Tuning automatico in base al sample rate (len 192-256, count 10-12) e chunk dinamici 2x DMA buffer per ridurre blocchi

### 1.3 Memory Management Enhancement
- [x] **Memory pooling**: Buffer PCM/leftover pre-allocati e riutilizzati tra playback (SPIRAM) per ridurre frammentazione
- [x] **Defragmentazione PSRAM**: Fallback di allocazione ring buffer con resize graduale e switch a DRAM se necessario
- [x] **Monitor memory leaks**: Tracking heap baseline/min e comando seriale `m` + log delta start/stop
- [x] **Configurazioni multiple**: Preset compile-time `AUDIO_PRESET_LOW_MEM` (ring 64KB PSRAM, target buffer 250ms, chunk 512, stack ridotti)

## 🚀 Fase 2: Feature Avanzate (Completata ✅)

### 2.1 Playback Controls Runtime ✅
- [x] **Volume control**: API per controllo volume runtime (0-100) via `v75`
- [x] **Pause/Resume**: Implementazione pause senza perdere sincronismo
- [x] **Seek functionality**: Brute force seeking per posizione secondi `s123`
- [x] **Stop/Restart**: Gestione completa ciclo di vita playback

### 2.2 Stato e Monitoraggio ✅
- [x] **State machine**: Stati enum (STOPPED/PLAYING/PAUSED/ENDED/ERROR)
- [x] **Performance metrics**: Mem usage e uptime logging
- [x] **Metadata extraction**: Lettura ID3v1/v2 (TIT2/TPE1/TALB/TCON/TRCK/TDRC/COMM/TXXX, UTF-8/ISO/UTF-16) + rilevamento cover (APIC) via comando di load
- [x] **Progress tracking**: Progress secondi ogni 5s (posizione/durata)

### 2.3 Serial Control Interface ✅
- [x] **Comandi seriali**: Parse completo comandi ASCII (l/p/r/q/v##/s##/h/t)
- [x] **Status reporting**: Output stato/metrics runtime (basic, comando `i`)
- [x] **Selezione file runtime**: Comando `f<path>` per scegliere l'MP3 e `t` per ripristinare il test file
- [x] **File listing**: Lista file in LittleFS via comando `d` (root) o `d/path`
- [ ] **Configurazione live**: Parametri runtime (core implemented)

### 2.4 Known Issues & Optimizations 🚧
- **Stop manuale/pausa lunga**: ✅ Risolto — stop in pausa ora attende conferma task e ripristina volume.
- **Stop post-EOF**: In alcuni casi lo stop comandato dopo EOF forza delete per timeout EventGroup (mitigato con controllo handle nullo; da validare con test lunghi)
- **Pause lunghe**: WARN ripetuti ring buffer quando buffer vuoto in pausa (non critico)
- **Ring buffer quasi pieno by design**: Con soglia 16KB su ring piccolo (DRAM) i WARN appaiono spesso; regolare `producer_min_free_bytes`/chunk per silenziare.
- **I2S write error spurio**: Log `[ERROR] Errore scrittura I2S: ESP_OK` dopo riavvii rapidi → verificare bytes_written/timeout e stato driver.
- **Buffer dynamic sizing**: Adattamento dinamica dimensioni ring buffer
- **Task priorities**: Ottimizzazione CPU usage
- **I2S latency tuning**: Ridurre latenza DMA buffer

## 🏗️ Fase 3: Refactoring e Modularizzazione (1-2 settimane)

### 3.1 Code Architecture
- [ ] **Class-based design**: Convertire da funzioni globali a classe AudioPlayer
- [x] **Configurazione strutturata**: Struct per tutti parametri configurabili (buffer, stack, timeouts)
- [x] **Callback system**: Sistema di callback per eventi (start/stop/end/error/metadata) registrabile a runtime
- [ ] **Task management**: Classe wrapper per gestione sicura task FreeRTOS

### 3.2 Separate Components
```
src/
├── audio_player.h/cpp      # Main interface
├── mp3_decoder.h/cpp       # dr_mp3 wrapper
├── codec_driver.h/cpp      # ES8311 wrapper
├── ring_buffer_stream.h/cpp # Producer-consumer abstraction
├── file_streamer.h/cpp     # LittleFS streaming
├── audio_config.h          # Configuration structs
└── main.cpp               # Application logic
```

### 3.3 Build System
- [ ] **Componenti PlatformIO**: Separare in sottocartelle con propri CMakeLists.txt
- [ ] **Unit tests**: Framework per testing componenti isolati
- [ ] **CI/CD basic**: Script per build automatizzato e testing
- [ ] **Header guards**: Protezioni multiple inclusion coerenti

## 📊 Fase 4: Quality Assurance e Validation (1 settimana)

### 4.1 Testing Completo
- [ ] **Audio quality tests**: Comparazione A/B con riferimento
- [ ] **Stress testing**: File grandi, interrupt frequenti
- [ ] **Edge cases**: File corrotti, filesystem pieno
- [ ] **Memory pressure**: Testing su dispositivi low-memory

### 4.2 Documentazione Finale
- [ ] **API documentation**: Doxygen per tutte le funzioni pubbliche
- [ ] **Troubleshooting guide**: Database problemi comuni e soluzioni
- [ ] **Performance benchmarks**: Metriche comparate con libreria GPL
- [ ] **Migration guide**: Come passare dalla libreria GPL

### 4.3 Code Quality
- [ ] **Static analysis**: ESP-IDF static analyzer
- [ ] **Memory sanitizer**: Valgrind-esque per ESP32
- [ ] **Code formatting**: Unificare stile con clang-format
- [ ] **Dependency audit**: Verifica licenze di tutti i file inclusi

## 🔮 Fase 5: Estensioni Future (3+ mesi)

### 5.1 Supporto Formati Multipli
- [ ] **WAV/PCM**: Supporto file WAV non-compressi
- [ ] **FLAC/Ogg**: Vorbis decoder Apache 2.0 compatible
- [ ] **Format detection**: Auto-detection basato su magic bytes
- [ ] **Playlist support**: Multiple file in sequenza

### 5.2 Audio Input/Riproduzione e Streaming
- [ ] **HTTP Streaming**: Riproduzione diretta da URL web (no download completo)
- [ ] **Scheda SD Support**: Integrazione SDMMC per filesystem espandibile (alternativa LittleFS)
- [ ] **Microphone recording**: Uso ES8311 come ADC per registrazione
- [ ] **Echo effect**: Loopback microfono → speaker
- [ ] **Bluetooth audio**: Supporto A2DP per streaming wireless
- [ ] **Voice commands**: Wake word detection (se risorse permettono)

### 5.3 Playback Advanced Features
- [ ] **Playback Speed Control**: Velocità variabile (0.5x-2x) con pitch correction audio
- [ ] **Reverse Playback**: Riproduzione al contrario con bufferring invertito

### 5.4 Advanced Features
- [ ] **Equalizer**: Filtri digitali per tono/bassi/alti
- [ ] **Volume fading**: Fade in/out automatico
- [ ] **Gapless playback**: Transizione seamless tra tracce
- [ ] **Resume capability**: Salvataggio stato su NVS per resume dopo reboot

### 5.5 Ottimizzazioni Avanzate
- [ ] **SIMD optimization**: Uso ESP32 vector instructions per DSP
- [ ] **Hardware acceleration**: Uso IPU per decodifica se disponibile
- [ ] **Power management**: Clock scaling durante pausa/inattività
- [ ] **Multi-core usage**: U1 core dedicato per decodifica

## 📋 Metriche di Successo

### Performance Targets
- **CPU Usage**: < 20% per MP3 44.1kHz stereo
- **Memory Usage**: < 100KB PSRAM per decoder + buffers
- **Latency**: < 100ms da start file a primo suono
- **Underruns**: 0 durante riproduzione normale

### Quality Targets
- **Audio Quality**: Trasparente (no differenze udibili da source)
- **Reliability**: >99.9% uptime senza crash/restart
- **Compatibility**: Supporto 8kHz-48kHz, mono/stereo
- **Licensing**: 100% Apache 2.0 compliant

## ⏱️ Timeline Stimate
- **Fase 1**: 1-2 settimane (ottimizzazioni core)
- **Fase 2**: 2-3 settimane (feature avanzate)
- **Fase 3**: 1-2 settimane (refactoring)
- **Fase 4**: 1 settimana (QA)
- **Fase 5**: 3+ mesi (estensioni future)

*Totale: ~2-3 mesi per implementazione completa, testata e documentata*

## 🤝 Contributing
Questa roadmap è dinamica. Priorità possono essere riordinate basandosi su feedback utenti e testing. Le estensioni future sono pensate come moduli opzionali/plugin.

---
*Ultimo aggiornamento: Novembre 2025*
