# Glossario - openESPaudio

Termini tecnici e concetti chiave utilizzati nella documentazione.

## A

**Auto-pausa (Auto-pause)**: Funzione che mette automaticamente in pausa la riproduzione quando il buffer timeshift è insufficiente per connessioni lente.

## B

**Bitrate**: Velocità di trasmissione dati audio, misurata in kbps (kilobit per secondo). Es: MP3 128kbps.

**Buffer circolare (Circular buffer)**: Struttura dati che sovrascrive i dati più vecchi quando pieno, ideale per streaming continuo.

## C

**Chunk**: Unità indivisibile di dati audio (128KB-512KB) nel sistema timeshift. Ogni chunk è atomico e contiene una porzione continua di stream.

**Codec**: Dispositivo hardware/software che codifica/decodifica segnali audio digitali. Es: ES8311, PCM5102.

## D

**Decoder**: Componente software che converte formato compresso (MP3) in PCM audio.

**Downmixing**: Conversione audio multicanale a stereo o mono.

## E

**ESP32-S3**: Microcontrollore con supporto PSRAM, WiFi e I2S usato come piattaforma target.

## F

**Factory Pattern**: Design pattern che crea oggetti senza specificare la classe esatta da istanziare.

## H

**Heap**: Memoria dinamica allocata runtime. Su ESP32 limitata (~350KB), PSRAM estende significativamente.

**HTTP Streaming**: Trasmissione audio via protocollo HTTP, tipicamente per radio online.

## I

**I2S**: Interfaccia seriale per audio digitale, collega ESP32 al codec audio.

**IDataSource**: Interfaccia astratta che rappresenta qualsiasi sorgente dati (file, HTTP, timeshift).

## L

**LittleFS**: Filesystem leggero per flash interna ESP32, usato per file audio locali.

## M

**Metadata**: Informazioni descrittive associate al file audio (titolo, artista, durata, etc.).

**Mutex**: Meccanismo di sincronizzazione che previene accesso concorrente a risorse condivise.

## P

**PCM (Pulse Code Modulation)**: Formato audio non compresso (raw), output finale di tutti i decoder.

**PSRAM**: Pseudo Static RAM, memoria aggiuntiva veloce su ESP32-S3 (~2-8MB), ideale per buffer audio.

## R

**RAII (Resource Acquisition Is Initialization)**: Pattern C++ che assicura cleanup automatico risorse.

**Ring Buffer**: Buffer circolare per smoothing differenze velocità tra producer e consumer.

## S

**Sample Rate**: Frequenza campionamento audio (44.1kHz tipico per CD quality).

**Seek**: Saltare a posizione specifica nel file audio o buffer timeshift.

**Semaphore**: Contatore che controlla accesso a risorse condivise tra task.

## T

**Task (FreeRTOS)**: Thread leggero che esegue funzioni specifiche in parallelo.

**Timeshift**: Funzionalità che permette pausa/riavvolgimento stream live tramite buffer.

## U

**URI (Uniform Resource Identifier)**: Stringa che identifica una risorsa (file path o URL).

## V

**Volume**: Livello amplificazione audio, 0-100% in openESPaudio.

## W

**WAV**: Formato audio non compresso PCM, sempre seekable e semplice da decodificare.

## Simboli

**~**: Approssimativamente (es: ~2 minuti = circa 2 minuti).

**KB/MB**: Unità memoria (1024 bytes = 1KB, 1024KB = 1MB).

## Acronimi Comuni

**API**: Application Programming Interface - Interfaccia programmazione applicazioni.

**CPU**: Central Processing Unit - Unità di elaborazione centrale.

**FAT32**: File Allocation Table 32-bit - Filesystem per SD card.

**FIFO**: First In First Out - Politica accodamento.

**GPIO**: General Purpose Input Output - Pin configurabili ESP32.

**I/O**: Input/Output - Operazioni di ingresso/uscita.

**ISR**: Interrupt Service Routine - Funzione gestore interrupt.

**LED**: Light Emitting Diode - Diodo emettitore luce.

**MP3**: MPEG-1 Audio Layer III - Formato audio compresso popolare.

**RTOS**: Real Time Operating System - Sistema operativo tempo reale.

**SD**: Secure Digital - Tipo memoria flash per card.

**SPI**: Serial Peripheral Interface - Bus seriale per SD card.

**UART**: Universal Asynchronous Receiver Transmitter - Interfaccia seriale.

**USB**: Universal Serial Bus - Bus seriale universale.

**WiFi**: Wireless Fidelity - Tecnologia connessione wireless.
