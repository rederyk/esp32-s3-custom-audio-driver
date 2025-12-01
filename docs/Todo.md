# Todo - Stato Attuale

## ✅ Completati
- Modularizzazione AudioPlayer (AudioStream + AudioOutput)
- Supporto multi-formato (MP3, WAV)
- Implementazione PSRAM per timeshift
- Architettura timeshift finale con seek temporale
- Estrazione bitrate da header MP3
- Buffer safety (2 chunk minimum)
- Ottimizzazioni buffer dinamiche
- Fix timeout primo chunk

## 🔄 In Corso / Pianificati
* correggere wrappare init flush chunk timeshift
* creare funzione per estrarre registrazione.mp3 dal timeshift
* fallback chunk a indici presenti se no si trova quello indicato e wrappare le funzioni che fanno gia questo in altri modi

## 📚 Trasformazione in Libreria Arduino e Miglioramento Documentazione

### 🏗️ Trasformazione in Libreria Arduino
- [V] Spostare il core logico (AudioPlayer, AudioOutput, AudioStream, TimeshiftManager, etc.) in cartella src della libreria
- [V] Creare file library.properties per IDE Arduino e PlatformIO
- [V] Spostare main.cpp in examples/serial_control/serial_control.ino
- [V] Creare esempio play_local_file: examples/play_local_file/play_local_file.ino (riproduzione semplice da LittleFS/SD)
- [V] Creare esempio http_timeshift_radio: examples/http_timeshift_radio/http_timeshift_radio.ino (focalizzato su timeshift)
- [V] Creare esempio web_interface: examples/web_interface/web_interface.ino (controllo via AsyncWebServer)

### 📖 Miglioramento Documentazione
- [ ] Aggiornare README.md con struttura completa:
  - [ ] Titolo e breve descrizione: "openESPaudio: Una libreria audio avanzata per ESP32 con streaming HTTP, timeshift e altro"
  - [ ] Features: elenco puntato delle funzionalità chiave
  - [ ] Hardware Supportato: specifica scheda ESP32-S3 con codec ES8311 e pinout
  - [ ] Installazione: tramite Git clone o Library Manager
  - [ ] Quickstart / Esempio d'Uso: codice minimo per riprodurre un file
  - [ ] API Reference: descrizione classi principali (AudioPlayer, TimeshiftManager) e metodi pubblici
  - [ ] Documentazione Architetturale: link ai file .md esistenti (TIMESHIFT_ARCHITECTURE_FINAL.md, etc.)
