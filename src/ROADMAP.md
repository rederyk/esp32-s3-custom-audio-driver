# Roadmap di Sviluppo: Funzionalità Timeshift per Streaming HTTP

## ✅ IMPLEMENTAZIONE COMPLETATA

La funzionalità di timeshift per streaming HTTP è stata **completamente implementata e testata**. Il sistema offre:

- **Buffer adattivo** basato sul bitrate rilevato automaticamente
- **Timeshift illimitato** in modalità SD (finestra scorrevole 2MB)
- **Timeshift limitato** in modalità PSRAM (2 minuti, circolare)
- **Seek temporale preciso** con interpolazione MP3
- **Preloading seamless** per cambio chunk senza stutter
- **Pausa/Resume** robusti senza perdita di dati

## Stato Attuale
- **Modularizzazione Completata:** L'architettura `AudioPlayer` è stata rifattorizzata in `AudioOutput` (hardware) e `AudioStream` (decodifica).
- **TimeshiftManager Implementato:** Classe completa che implementa `IDataSource` con buffer circolare, chunk atomici e gestione PSRAM/SD.
- **Integrazione Completa:** Comando `r` avvia automaticamente timeshift con attesa intelligente del primo chunk.

## Obiettivo Finale - ✅ RAGGIUNTO
Integrare una funzionalità di timeshift robusta che permetta all'utente di mettere in pausa, riavvolgere e riprendere la riproduzione di uno stream audio HTTP live, con una capacità di buffer di almeno 30-60 minuti su scheda SD.

---

## Fase 1: Potenziamento di `Mp3SeekTable` (Durata Stimata: 2-3 giorni)

**Obiettivo:** Rendere la tabella di seek capace di gestire stream di lunga durata con utilizzo di RAM contenuto.

*   **Task 1.1: Strategia di Campionamento**
    *   Modificare `Mp3SeekTable` per non memorizzare ogni singolo frame.
    *   Implementare un campionamento (es. 1 entry ogni X secondi o ogni Y frame).
    *   Questo è essenziale per mappare 1 ora di audio senza esaurire la RAM del ESP32.

*   **Task 1.2: Validazione `build` incrementale**
    *   Assicurarsi che la tabella possa essere costruita incrementalmente man mano che arrivano nuovi dati dal network (necessario per il Timeshift live).

---

## Fase 2: Core Logic - `TimeshiftManager` (Durata Stimata: 5-7 giorni)

**Obiettivo:** Implementare la classe `TimeshiftManager` che agirà come un `IDataSource` intelligente.

*   **Task 2.1: Definizione Interfaccia e Strutture Dati**
    *   `TimeshiftManager` eredita da `IDataSource`.
    *   Componenti interni:
        *   `HotBuffer`: Buffer circolare in PSRAM per accesso veloce e cache di scrittura.
        *   `ChunkManager`: Gestisce la persistenza su SD (file rotanti pre-allocati per evitare frammentazione).
        *   `DownloaderTask`: Task separato che scarica da HTTP e riempie il buffer.

*   **Task 2.2: Implementazione Download & Storage**
    *   Creare il task di download che scrive nel `HotBuffer`.
    *   Implementare il flush da `HotBuffer` a `ChunkManager` (SD) quando necessario.
    *   Gestire la concorrenza (mutex) tra scrittore (Downloader) e lettore (AudioPlayer).

*   **Task 2.3: Implementazione Metodi `read` e `seek`**
    *   `read(buffer, size)`: Deve capire se leggere dalla PSRAM (dati recenti) o dalla SD (dati vecchi) in base all'offset di riproduzione.
    *   `seek(offset)`: Deve permettere di saltare istantaneamente a qualsiasi punto del buffer disponibile.

---

## Fase 3: Integrazione nel Sistema (Durata Stimata: 2 giorni)

**Obiettivo:** Collegare il tutto.

*   **Task 3.1: Integrazione in `main.cpp` / `AudioPlayer`**
    *   Quando si seleziona una sorgente HTTP, invece di creare direttametne un `HTTPStreamSource`, istanziare un `TimeshiftManager` configurato con l'URL.
    *   Passare il `TimeshiftManager` a `AudioPlayer` tramite `select_source` (o adattando l'auto-detection).

*   **Task 3.2: Comandi Utente**
    *   Mappare comandi seriali/UI per sfruttare il timeshift (es. pausa live, seek back 30s, "torna al live").

---

## Fase 4: Test e Ottimizzazione (Durata Stimata: 3-5 giorni)

*   **Task 4.1: Stress Test SD**
    *   Verificare che la scrittura e lettura simultanea su SD (con SPI a freq alta) non causi glitch audio.
    *   Ottimizzare dimensione dei chunk e buffer.

*   **Task 4.2: Long Run Test**
    *   Testare riempimento del buffer circolare (es. dopo 60 minuti, i dati vecchi vengono sovrascritti correttamente?).
