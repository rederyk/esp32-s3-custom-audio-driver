# 🎵 ESP32-S3 MP3 Audio Player 
Libreria audio completamente open-source come alternativa GPL-free alla ESP32-audioI2S, scritta da zero per ESP32-S3 con Freenove Display. Supporta MP3 stereo fino a 44.1kHz via LittleFS, con controllo completo e prestazioni ottimali.

## 📋 Panoramica

Implementazione professionale basata su classe `AudioPlayer` con due task FreeRTOS per streaming efficiente:

### Componenti Principali
- **AudioPlayer**: Classe principale con API pulita per controllo riproduzione
- **file_stream_task**: Streaming MP3 da LittleFS → ring buffer producer-consumer
- **audio_task**: Decodifica dr_mp3 + output I2S consumer

### Caratteristiche Tecniche
- **Decodificatore**: dr_mp3 (pubblico dominio/MIT-0)
- **Codec**: ES8311 integrato (configurato da I2C/SCLK)
- **Filesystem**: LittleFS flash (5MB partizionati)
- **Buffer**: Ring buffer PSRAM/DRAM ottimizzato dinamicamente
- **Formato**: MP3 stereo (8-48kHz), con metadata ID3v1/v2
- **Controllo**: Serial commands per play/stop/pause/seek/volume/file

## Schema Elettrico

| Componente | Pin ESP32-S3 | Funzione |
|------------|--------------|----------|
| ES8311 I2C | SDA=16, SCL=15 | Configurazione codec |
| ES8311 I2S | BCK=5, WS=7, DOUT=8 | Dati audio |
| Amplifier Enable | GPIO 1 | Abilitazione amplificatore (LOW attivo) |

## Firmware Setup

### Dipendenze
- **PlatformIO**: Framework Arduino con core ESP32-S3
- **Librerie**: Es8311 (driver codec), dr_mp3 (decodificatore single-file)

### Partizionamento Flash
```
LittleFS: 0x4F0000 bytes (~5MB) per file MP3
```

### Configurazione PlatformIO
```ini
[env:freenove-esp32-s3-display]
platform = espressif32
board = esp32-s3-devkitm-1
framework = arduino
lib_deps =
# Le dipendenze sono incluse nel progetto (es8311.*, dr_mp3.h)

monitor_speed = 115200
monitor_rts = 0
monitor_dtr = 0

board_build.filesystem = littlefs
board_build.partitions = partitions.csv
board_build.arduino.memory_type = qio_opi
board_build.psram.enable = true
board_build.psram.mode = opi
board_build.psram.freq = 80
```

## Utilizzo

### Preparazione file MP3
1. Copiare file MP3 in `data/test.mp3`
2. MP3 deve essere stereo 44.1kHz per compatibilità ottimale

### Caricamento
```bash
# Compila e carica firmware
pio run -t upload

# Carica filesystem LittleFS (insert adipisci una volta)
pio run -t uploadfs
```

### Debug Seriale
```bash
pio device monitor
```

### Output Seriale Atteso
```
=== BOOT: Test audio locale da LittleFS con dr_mp3 e driver ESP-IDF ===
Riproduzione da LittleFS: /test.mp3, size=2396995 bytes
Decodificatore MP3 inizializzato. Canali: 2, Sample Rate: 44100
ES8311 pronto.
Driver I2S installato per 44100 Hz, 16 bit, Stereo
Inizio decodifica e riproduzione...
[Loop] Uptime: 5 s, Heap Libero: 8535879 bytes
[Loop] Uptime: 10 s, Heap Libero: 8535879 bytes
...
Fine decodifica (stream terminato?).
Audio task terminato.
```

## Ottimizzazioni Implementate

### Memory Pooling
- **PSRAM**: Decoder drmp3 (~30KB) allocato in PSRAM esterna
- **Buffers**: PCM float/int16 allocati in PSRAM per conservazione RAM interna
- **Ring Buffer**: 32KB byte buffer per streaming efficiente

### Task Prioritization
- Audio task priorità 5 (alta) per audio glitch-free
- File task priorità 5 per I/O continuo

### Streaming Efficiency
- Read buffer da 1KB da LittleFS al ring buffer
- Decode buffer da 4096 float samples (2048 frames stereo)
- I2S DMA: 8 buffer da 512 samples cadaun

## Risoluzione Problemi

### Problema: Nessun suono nonostante inizializzazione OK
**Soluzione**: Verificare `AP_ENABLE` pin 1 impostato a LOW (era HIGH nel codice originale)

### Problema: Audio distorto ("tum tum")
**Soluzione**: Aggiunto clamping PCM per prevenire overflow:
```cpp
float sample = pcm_float[i] * 32767.0f;
if (sample > 32767.0f) sample = 32767.0f;
else if (sample < -32768.0f) sample = -32768.0f;
pcm_s16[i] = (int16_t)sample;
```

### Problema: "es8311_create failed"
**Soluzione**: Verificare collegamenti I2C, alimentare scheda correttamente

### Problema: LittleFS mount fallito
**Soluzione**: `pio run -t uploadfs` per caricare filesystem

## Considerazioni di Performance

### Consumo Memoria (circa 65KB PSRAM)
- drmp3 decoder: ~30KB
- Buffer PCM float: 32KB (4096 * 4 * 2)
- Buffer PCM int16: 32KB (4096 * 2 * 2)
- Ring buffer: 32KB

### CPU Usage
- ~15% durante decodifica/riproduzione
- Task audio: 32KB stack (massima sicurezza)
- Task file: 4KB stack (read-only)

## Differenze dalla Implementazione Ufficiale Freenove

| Aspecto | Questa Implementazione | Tutorial Ufficiale |
|---------|----------------------|-------------------|
| **Source File** | LittleFS | MicroSD card |
| **Decoder Lib** | dr_mp3 (single-file) | ESP32-audioI2S |
| **Memory Usage** | ~1MB meno | Maggiore overhead libreria |
| **Heap Retention** | 8.5MB+ libera | Meno efficiente |
| **Setup Complexity** | Alta (custom) | Semplice (libreria pronta) |
| **Controllo Fine** | Completo | Mediano |

## Licenze
- **dr_mp3**: Pubblico dominio o MIT-0
- **Es8311 Driver**: Freenove (basato open-source)
- **Codice Progetto**: MIT (assumed)

## Versioni
- dr_mp3: v0.7.2
- Framework: Arduino ESP32 3.0+
- ESP-IDF: Integrato via PlatformIO

---

Per ulteriori dettagli consultare il codice sorgente e commenti inline.
