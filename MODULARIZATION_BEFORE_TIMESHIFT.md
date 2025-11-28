## Architettura AudioPlayer Completata

Il refactoring di modularizzazione è stato completato. La classe `AudioPlayer` è stata scomposta in tre componenti principali per separare le responsabilità:

1. **AudioStream**: Gestisce la decodifica da `IDataSource` a flusso PCM, supportando formati multipli (MP3, WAV) tramite decoder polimorfici.
2. **AudioOutput**: Incapsula l'interazione con l'hardware audio (Codec ES8311 e I2S driver).
3. **AudioPlayer**: Orchestratore di alto livello che gestisce lo stato di riproduzione e coordina AudioStream e AudioOutput.

Questa architettura modulare ha reso il codice più testabile e maneggevole, facilitando l'implementazione di funzionalità avanzate come il timeshift.
