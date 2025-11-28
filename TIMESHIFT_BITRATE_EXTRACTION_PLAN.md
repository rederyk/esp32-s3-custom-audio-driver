# Piano di implementazione: Estrazione del bitrate reale da header MP3

## Obiettivo
Utilizzare sempre il bitrate reale ricavato dal primo header MP3 per dimensionare dinamicamente i buffer nel `TimeshiftManager`, evitando discrepanze tra throughput stimato e bitrate del flusso.

## Passaggi

1. **Aggiornare la firma di `calculate_chunk_duration`**
   - Aggiungere un parametro di output `uint32_t& out_bitrate_kbps`.
   - Documentare la nuova firma:
     ```cpp
     bool calculate_chunk_duration(const ChunkInfo& chunk,
                                   uint32_t& out_frames,
                                   uint32_t& out_duration_ms,
                                   uint32_t& out_bitrate_kbps);
     ```

2. **Estrarre il bitrate dal primo header MP3**
   - All’inizio di `calculate_chunk_duration`, leggere i primi 4 byte e identificare il sync word.
   - Usare la tabella `bitrate_table` per calcolare `out_bitrate_kbps`.
   - Marcare un flag `header_detected = true` per eseguire una sola volta la dimensione iniziale.

3. **Dimensionamento dinamico iniziale su header**
   - Se `!bitrate_adapted_once_` e `out_bitrate_kbps > 0`, chiamare:
     ```cpp
     calculate_adaptive_sizes(out_bitrate_kbps);
     bitrate_adapted_once_ = true;
     ```
   - In questo modo il sizing dei buffer usa il bitrate reale.

4. **Disabilitare il matching sui `common_bitrates` per sizing iniziale**
   - Conservare la logica di throughput in `apply_bitrate_measurement` come fallback, ma non sovrascrivere il sizing fatto dall’header.

5. **Adeguare `promote_chunk_to_ready` (se necessario)**
   - Verificare se serve adattare ulteriormente la logica di promozione dei chunk in base al nuovo sizing.

6. **Verifiche e test**
   - Ricompilare il firmware e avviare il logging in DEBUG.
   - Effettuare streaming MP3 a bitrate noto (ad esempio 192 kbps) e controllare nei log:
     - Chiamata a `calculate_adaptive_sizes(192)`.
     - Nessun successivo override iniziale a 320 kbps.
   - Ripetere il test con flussi a 128 kbps e 320 kbps.

7. **Documentazione finale**
   - Aggiornare i commenti di `calculate_chunk_duration` e `apply_bitrate_measurement`.
   - Aggiungere una sezione nel documento d’architettura (`TIMESHIFT_ARCHITECTURE_FINAL.md`) che descriva il flusso di sizing basato su header MP3.

---

**Risultato atteso:** il TimeshiftManager dimensiona i buffer iniziali con l’esatto bitrate del flusso MP3, garantendo stabilità e assenza di errori di overruns o underruns durante il cambio contesto.
