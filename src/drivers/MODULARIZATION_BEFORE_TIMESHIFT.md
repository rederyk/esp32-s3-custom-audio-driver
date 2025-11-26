## Roadmap di Refactoring: Modularizzazione pre-Timeshift

### Obiettivo Finale:
Ristrutturare la classe `AudioPlayer` per separare le responsabilità di decodifica, gestione dell'hardware audio e controllo dello stato. Questo refactoring è un prerequisito fondamentale per l'implementazione della funzionalità di timeshift, poiché renderà il codice più maneggevole, testabile e modulare.

L'idea centrale è di scomporre il monolitico `AudioPlayer` in tre componenti principali:
1.  `AudioStream`: Responsabile della decodifica di un `IDataSource` in un flusso di dati PCM.
2.  `AudioOutput`: Responsabile della gestione dell'hardware audio (Codec e I2S) e della riproduzione del flusso PCM.
3.  `AudioPlayer`: Diventa un orchestratore di alto livello che gestisce lo stato (play, pausa, seek) e coordina gli altri due componenti.

---

### Fase 1: Creazione del Modulo `AudioOutput` (Durata Stimata: 1 giorno)

**Obiettivo:** Incapsulare tutta la logica di interazione con l'hardware audio in una classe dedicata.

*   **Task 1.1: Definizione dell'Interfaccia `AudioOutput`**
    *   Creare `src/audio_output.h`.
    *   Definire la classe `AudioOutput` con i seguenti metodi pubblici:
        *   `bool begin(uint32_t sample_rate, uint32_t channels, ...)`: Inizializza `I2sDriver` e `CodecES8311`.
        *   `void end()`: Disinstalla il driver I2S.
        *   `size_t write(const int16_t* data, size_t frames_to_write)`: Scrive i campioni PCM al driver I2S.
        *   `void set_volume(int percent)`: Imposta il volume sul codec.
        *   `void stop()`: Pulisce i buffer DMA di I2S (utile per il seek).

*   **Task 1.2: Implementazione di `AudioOutput`**
    *   Creare `src/audio_output.cpp`.
    *   Spostare le istanze di `I2sDriver` e `CodecES8311` da `AudioPlayer` a `AudioOutput`.
    *   Spostare la logica di inizializzazione e configurazione di I2S e codec da `AudioPlayer::audio_task` a `AudioOutput::begin`.
    *   Spostare la logica di `i2s_write` da `AudioPlayer::audio_task` a `AudioOutput::write`.

*   **Task 1.3: Integrazione in `AudioPlayer`**
    *   In `audio_player.h`, sostituire le istanze di `I2sDriver` e `CodecES8311` con un'unica istanza di `AudioOutput`.
    *   In `audio_player.cpp`, modificare `audio_task` per chiamare `output.begin()`, `output.write()` e `output.end()`.
    *   Modificare `set_volume()` e `toggle_pause()` per delegare il controllo del volume a `output.set_volume()`.

*   **Task 1.4: Test di Regressione**
    *   Compilare e testare la riproduzione da file (LittleFS/SD) e stream HTTP.
    *   Verificare che play, pausa, stop e controllo volume funzionino come prima.

---

### Fase 2: Creazione del Modulo `AudioStream` (Durata Stimata: 1-2 giorni)

**Obiettivo:** Isolare la logica di decodifica MP3 in una classe dedicata.

*   **Task 2.1: Definizione dell'Interfaccia `AudioStream`**
    *   Creare `src/audio_stream.h`.
    *   Definire la classe `AudioStream` con i seguenti metodi pubblici:
        *   `bool begin(std::unique_ptr<IDataSource> source)`: Prende possesso della sorgente dati e inizializza `Mp3Decoder`.
        *   `void end()`: Rilascia le risorse del decoder.
        *   `size_t read(int16_t* buffer, size_t frames_to_read)`: Legge e decodifica i dati, restituendo campioni PCM.
        *   `bool seek(uint64_t pcm_frame_index)`: Esegue il seek nel flusso.
        *   Metodi getter per `sample_rate`, `channels`, `total_frames`, ecc.

*   **Task 2.2: Implementazione di `AudioStream`**
    *   Creare `src/audio_stream.cpp`.
    *   Spostare l'istanza di `Mp3Decoder` da `AudioPlayer` a `AudioStream`.
    *   Spostare la logica di `decoder_.init()` e `decoder_.read_frames()` da `AudioPlayer::audio_task` ai metodi `begin()` e `read()` di `AudioStream`.

*   **Task 2.3: Integrazione in `AudioPlayer`**
    *   In `audio_player.h`, sostituire l'istanza di `Mp3Decoder` con `std::unique_ptr<AudioStream>`.
    *   In `AudioPlayer::start()`, creare l'istanza di `AudioStream` e passargli la `data_source_` con `std::move`.
    *   Semplificare drasticamente `AudioPlayer::audio_task`: il loop principale ora consisterà in:
        1.  `stream_->read(...)` per ottenere dati PCM.
        2.  `output_.write(...)` per riprodurli.
    *   Modificare `request_seek()` per delegare la chiamata a `stream_->seek()`.

*   **Task 2.4: Test di Regressione Completo**
    *   Compilare e testare nuovamente tutte le funzionalità.
    *   Verificare che il seek su file funzioni correttamente.
    *   Assicurarsi che la gestione degli stati (play, pausa, stop, fine traccia) sia corretta.

---

### Fase 3: Finalizzazione e Pulizia (Durata Stimata: <1 giorno)

**Obiettivo:** Rimuovere codice obsoleto e verificare la pulizia della nuova architettura.

*   **Task 3.1: Revisione di `AudioPlayer`**
    *   Rimuovere tutti i membri e le variabili locali non più necessarie (es. buffer PCM interni al task, riferimenti diretti a `decoder_`, ecc.).
    *   Assicurarsi che `AudioPlayer` gestisca solo lo stato e l'orchestrazione.

*   **Task 3.2: Conclusione**
    *   A questo punto, il codice è pronto. L'implementazione del `TimeshiftManager` potrà essere realizzata come una classe `IDataSource` specializzata, che verrà passata ad `AudioStream` senza che `AudioPlayer` debba conoscerne i dettagli.