# Roadmap di Sviluppo: Funzionalità Timeshift per Streaming HTTP

## Obiettivo Finale:
Integrare una funzionalità di timeshift robusta che permetta all'utente di mettere in pausa, riavvolgere e riprendere la riproduzione di uno stream audio HTTP live, con una capacità di buffer di almeno 30-60 minuti.

---

## Fase 0: Preparazione e Fondamenta (Durata Stimata: 1-2 giorni)

**Obiettivo:** Preparare l'ambiente di sviluppo, integrare le dipendenze e validare i componenti di base.

*   **Task 1: Setup del Branch di Sviluppo**
    *   Creare un nuovo branch Git dedicato alla feature (es. `feature/timeshift`) per isolare lo sviluppo.

*   **Task 2: Integrazione Librerie**
    *   Integrare la libreria `dr_mp3.h` nel progetto. Questa libreria è essenziale per un parsing efficiente e affidabile dei frame MP3.
    *   Verificare che tutte le dipendenze necessarie per `TimeshiftManager` (FreeRTOS, HTTPClient, WiFi) siano correttamente configurate.

*   **Task 3: Validazione I/O di Base**
    *   Creare un piccolo test per confermare le performance di lettura/scrittura della scheda SD, assicurandosi che siano adeguate per la registrazione in tempo reale di uno stream a 128-320 kbps.

---

## Fase 1: Adattamento dei Componenti Esistenti (Durata Stimata: 2-3 giorni)

**Obiettivo:** Modificare la classe `Mp3SeekTable` per renderla capace di operare su file di grandi dimensioni senza consumare eccessiva RAM.

*   **Task 1: Implementazione del Nuovo Metodo `build`**
    *   Finalizzare e testare l'implementazione del metodo `bool build(IDataSource* source, ...)` in `mp3_seek_table.cpp`. L'obiettivo è validarlo e assicurarsi che gestisca correttamente i casi limite (file vuoto, file corrotto, ecc.).

*   **Task 2: Test Unitari per `Mp3SeekTable`**
    *   **Punto di Attenzione:** Per evitare un consumo eccessivo di RAM, considerare un'implementazione della tabella di seek a campionamento (es. un punto di ingresso ogni 2-10 secondi) invece di memorizzare ogni frame. Questo offre un buon compromesso tra precisione del seek e uso della memoria.

*   **Task 3: Test Unitari per `Mp3SeekTable`**
    *   Scrivere un test specifico che:
        1.  Crea un file MP3 di test sulla scheda SD.
        2.  Crea un `DataSource` per quel file.
        3.  Chiama il nuovo metodo `build(dataSource, ...)` su un'istanza di `Mp3SeekTable`.
        4.  Verifica che la tabella di seek venga costruita correttamente.

---

## Fase 2: Sviluppo del Core Logic (`TimeshiftManager`) (Durata Stimata: 5-7 giorni)

**Obiettivo:** Implementare e stabilizzare il `TimeshiftManager`, il cuore della funzionalità, basandosi sull'architettura definita nei file `timeshift_manager.h` e `.cpp`.

*   **Task 1: Revisione e Finalizzazione dell'Architettura**
    *   Analizzare l'architettura esistente basata su `HotBuffer` (PSRAM), `ChunkManager` (SD) e `ProgressiveSeekTable`. Il lavoro consiste nel testarla, debuggarla e rifinirla.

*   **Task 1.1: Analisi dei Rischi Architetturali e Mitigazioni**
    *   **Rischio 1: Race Condition e Deadlock.** L'accesso concorrente al buffer e ai chunk da parte del task di prefetch, del lettore audio e del task di pulizia è critico.
        *   **Mitigazione:** Implementare una strategia di sincronizzazione robusta, ad esempio utilizzando un singolo mutex per proteggere tutte le strutture dati condivise (buffer, metadati dei chunk, offset). Gestire gli stati dei chunk in modo atomico per evitare conflitti tra lettura e cancellazione.

    *   **Rischio 2: Frammentazione della Scheda SD.** La creazione e cancellazione continua di file di chunk può degradare le performance di I/O nel tempo.
        *   **Mitigazione:** Implementare il `ChunkManager` utilizzando un set di file pre-allocati gestiti come un buffer circolare (ring buffer). Questo elimina la frammentazione del filesystem.

    *   **Rischio 3: Latenza di Archiviazione (PSRAM -> SD).** La scrittura su SD è lenta e potrebbe bloccare l'accesso al `HotBuffer`, causando un underrun del player.
        *   **Mitigazione:** Considerare un meccanismo di "double buffering" per l'archiviazione. I dati da archiviare vengono copiati in un buffer temporaneo, permettendo al task di scrittura su SD di operare senza bloccare il `HotBuffer` principale.

*   **Task 2: Implementazione e Test del Task di Prefetch (`prefetch_loop`)**
    *   Focalizzarsi sul task che scarica i dati dalla rete.
    *   Verificare che gestisca correttamente le disconnessioni e le riconnessioni.
    *   Assicurarsi che scriva i dati in modo concorrente e sicuro nel `HotBuffer` e nel `ChunkManager`.
    *   Implementare la logica di archiviazione: quando i dati nel `HotBuffer` superano una soglia (`archive_threshold`), devono essere scritti in un chunk su SD.

*   **Task 3: Implementazione dell'Interfaccia `IDataSource`**
    *   Implementare i metodi `read()` e `seek()` del `TimeshiftManager`.
    *   `read()`: Deve essere *thread-safe* e leggere i dati in modo trasparente dal `HotBuffer` o, se necessario, dai chunk su SD.
    *   `seek()`: Deve essere *thread-safe* e permettere di posizionare il `playback_offset_` in qualsiasi punto valido del buffer totale.

*   **Task 4: Gestione del Ciclo di Vita dei Dati**
    *   Implementare la logica di `cleanup_old_data()`. Il sistema deve cancellare periodicamente i chunk più vecchi sulla SD per rispettare la finestra di timeshift definita.
    *   **Punto di Attenzione:** Questa operazione deve essere sincronizzata per evitare di cancellare un chunk mentre è in uso dal lettore.

---

## Fase 3: Integrazione e UI (Durata Stimata: 3-4 giorni)

**Obiettivo:** Integrare il `TimeshiftManager` nel player audio principale e collegarlo ai controlli dell'interfaccia utente.

*   **Task 1: Sostituzione della Sorgente Dati**
    *   Modificare il codice del player audio per istanziare e utilizzare `TimeshiftManager` come sua `IDataSource` primaria.

*   **Task 2: Implementazione dei Controlli Utente**
    *   Collegare i comandi dell'interfaccia utente (es. pulsanti, comandi web) alle funzioni del `TimeshiftManager`:
        *   **Pausa/Play:** Chiama `set_mode(Mode::PAUSED)` e `set_mode(Mode::TIMESHIFT)`.
        *   **Rewind/Forward:** Chiama `seek_relative_seconds()`.
        *   **Torna al Live:** Chiama `seek_to_live()`.

*   **Task 3: Visualizzazione dello Stato**
    *   Utilizzare la funzione `get_stats()` per visualizzare informazioni utili sull'interfaccia utente (display, pagina web), come il ritardo dal live e la durata del buffer.

---

## Fase 4: Test e Ottimizzazione (Durata Stimata: 4-5 giorni)

**Obiettivo:** Eseguire test intensivi sul campo per identificare bug, colli di bottiglia e ottimizzare le performance e la stabilità.

*   **Task 1: Test di Lunga Durata**
    *   Lasciare il dispositivo in esecuzione per 24-48 ore per verificare la presenza di memory leak o crash.

*   **Task 2: Stress Test**
    *   Eseguire ripetutamente e rapidamente comandi di pausa, play, seek e ritorno al live per scovare race condition.
    *   Testare con connessioni WiFi instabili per verificare la robustezza del buffer.
    *   **Scenari di Test Aggiuntivi:**
        *   **Test "Seek-to-Live":** Eseguire seek all'indietro seguiti immediatamente da un ritorno al live per testare la transizione tra lettura da SD e da PSRAM.
        *   **Test di Riempimento Buffer:** Lasciare il sistema in pausa fino al riempimento completo del buffer per verificare la stabilità e la corretta attivazione della pulizia.
        *   **Test con Bitrate Variabile (VBR):** Se possibile, usare uno stream VBR per validare la robustezza della logica di seek e parsing.

*   **Task 3: Ottimizzazione delle Performance**
    *   Profilare l'uso della CPU e della memoria.
    *   Ottimizzare i parametri di configurazione (`prefetch_chunk_size`, `archive_threshold`, ecc.) per trovare il miglior equilibrio tra reattività e consumo di risorse.

*   **Task 4: Merge Finale**
    *   Una volta che la feature è stabile e validata, eseguire il merge del branch `feature/timeshift` nel branch principale.