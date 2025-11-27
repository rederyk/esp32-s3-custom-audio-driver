#include "timeshift_manager.h"
#include "logger.h"
#include "drivers/sd_card_driver.h"  // For SD card access
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>  // For PSRAM allocation
#include "mp3_seek_table.h"

constexpr size_t BUFFER_SIZE = 128 * 1024;       // 128KB per buffer
constexpr size_t CHUNK_SIZE = 128 * 1024;        // Chunk fissi da 128KB
// CONFIGURABILE: Ridotto da 512MB a 256MB per cleanup più aggressivo
// Modifica questo valore per cambiare quando vengono rimossi i chunk vecchi
constexpr size_t MAX_TS_WINDOW = 1024 * 1024 * 2; // 256 MB max window (era 512MB)
constexpr size_t DOWNLOAD_CHUNK = 4096;          // Aumentiamo per efficienza
constexpr size_t MIN_CHUNK_FLUSH_SIZE = 124 * 1024; // Flush vicino alla dimensione target

TimeshiftManager::TimeshiftManager() {
    mutex_ = xSemaphoreCreateMutex();
    is_auto_paused_ = false;
}

TimeshiftManager::~TimeshiftManager() {
    stop();
    close();
    if (mutex_) vSemaphoreDelete(mutex_);
    if (recording_buffer_) free(recording_buffer_);
    if (playback_buffer_) free(playback_buffer_);
    free_psram_pool();
}

bool TimeshiftManager::open(const char* uri) {
    if (is_open_) close();

    uri_ = uri;
    is_open_ = true;
    current_recording_offset_ = 0;
    current_read_offset_ = 0;
    rec_write_head_ = 0;
    bytes_in_current_chunk_ = 0;
    next_chunk_id_ = 0;
    current_playback_chunk_id_ = INVALID_CHUNK_ID;
    playback_chunk_loaded_size_ = 0;
    pending_chunks_.clear();
    ready_chunks_.clear();
    pause_download_ = false;
    is_auto_paused_ = false;
    cumulative_time_ms_ = 0;  // Reset temporal tracking

    // Initialize storage backend based on current mode
    if (storage_mode_ == StorageMode::SD_CARD) {
        // Create timeshift directory if it doesn't exist
        if (!SD_MMC.exists("/timeshift")) {
            SD_MMC.mkdir("/timeshift");
            LOG_INFO("Created /timeshift directory");
        }

        // Clean up old timeshift files from previous sessions
        File tsDir = SD_MMC.open("/timeshift");
        if (tsDir && tsDir.isDirectory()) {
            File file = tsDir.openNextFile();
            while (file) {
                String fname = String("/timeshift/") + file.name();
                file.close();
                SD_MMC.remove(fname.c_str());
                LOG_DEBUG("Cleaned up: %s", fname.c_str());
                file = tsDir.openNextFile();
            }
            tsDir.close();
            LOG_INFO("Timeshift directory cleaned");
        }
        LOG_INFO("Timeshift mode: SD_CARD");
    } else {
        // PSRAM mode: allocate chunk pool
        if (!init_psram_pool()) {
            LOG_ERROR("Failed to initialize PSRAM pool");
            close();
            return false;
        }
        LOG_INFO("Timeshift mode: PSRAM_ONLY (%u chunks, %u KB total)",
                 MAX_PSRAM_CHUNKS, psram_pool_size_ / 1024);
    }

    recording_buffer_ = (uint8_t*)malloc(BUFFER_SIZE);
    if (!recording_buffer_) {
        LOG_ERROR("Failed to allocate recording buffer");
        close();
        return false;
    }

    playback_buffer_ = (uint8_t*)malloc(PLAYBACK_BUFFER_SIZE);
    if (!playback_buffer_) {
        LOG_ERROR("Failed to allocate playback buffer");
        free(recording_buffer_);
        recording_buffer_ = nullptr;
        close();
        return false;
    }

    LOG_INFO("Timeshift buffers allocated: rec=%uKB, play=%uKB",
             BUFFER_SIZE / 1024, PLAYBACK_BUFFER_SIZE / 1024);
    return true;
}

void TimeshiftManager::close() {
    stop();

    // Clean up all chunks based on storage mode
    if (storage_mode_ == StorageMode::SD_CARD) {
        for (const auto& chunk : pending_chunks_) {
            SD_MMC.remove(chunk.filename.c_str());
        }
        for (const auto& chunk : ready_chunks_) {
            SD_MMC.remove(chunk.filename.c_str());
        }
    } else {
        // PSRAM mode: pool will be freed, no per-chunk cleanup needed
    }

    pending_chunks_.clear();
    ready_chunks_.clear();

    if (recording_buffer_) {
        free(recording_buffer_);
        recording_buffer_ = nullptr;
    }

    if (playback_buffer_) {
        free(playback_buffer_);
        playback_buffer_ = nullptr;
    }

    // Free PSRAM pool if allocated
    free_psram_pool();

    is_open_ = false;
    seek_table_.clear();
}

bool TimeshiftManager::start() {
    if (!is_open_ || is_running_) {
        LOG_WARN("TimeshiftManager::start() - already open or running");
        return false;
    }

    is_running_ = true;
    BaseType_t result = xTaskCreate(download_task_trampoline, "ts_download", 8192, this, 5, &download_task_handle_);
    if (result != pdPASS) {
        LOG_ERROR("Failed to create download task");
        is_running_ = false;
        return false;
    }
    LOG_INFO("TimeshiftManager download task created successfully");

    // Crea il nuovo task di pre-caricamento
    result = xTaskCreate(preloader_task_trampoline, "ts_preloader", 4096, this, 4, &preloader_task_handle_);
    if (result != pdPASS) {
        LOG_ERROR("Failed to create preloader task");
        stop(); // Cleanup
        return false;
    }
    return true;
}

void TimeshiftManager::stop() {
    if (is_running_) {
        is_running_ = false;
        if (download_task_handle_) {
            vTaskDelete(download_task_handle_);
            download_task_handle_ = nullptr;
        }
        if (preloader_task_handle_) {
            vTaskDelete(preloader_task_handle_);
            preloader_task_handle_ = nullptr;
        }
    }
}

size_t TimeshiftManager::read(void* buffer, size_t size) {
    if (!is_open_) return 0;

    // --- ROBUSTO BUFFERING INIZIALE ---
    // Causa #5 & #6: Forziamo l'attesa di un buffer sano all'avvio.
    // Questo si applica solo alla primissima chiamata a read().
    if (current_read_offset_ == 0) {
        const size_t MIN_CHUNKS_FOR_START = 2;
        const uint32_t MAX_WAIT_MS = 15000; // Aumentato a 15s per sicurezza
        uint32_t start_wait = millis();

        while (is_running_ && ready_chunks_.size() < MIN_CHUNKS_FOR_START) {
            if (millis() - start_wait > MAX_WAIT_MS) {
                LOG_ERROR("Timeout waiting for initial buffer (%u chunks)", (unsigned)MIN_CHUNKS_FOR_START);
                return 0;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    // Se, dopo l'attesa, non ci sono chunk, è un errore grave o la fine dello stream.
    if (ready_chunks_.empty()) {
        LOG_WARN("No ready chunks available for playback. End of stream?");
        return 0;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);

    // Read from playback buffer (will load chunk if needed)
    size_t bytes_read = read_from_playback_buffer(current_read_offset_, buffer, size);
    if (bytes_read > 0) {
        current_read_offset_ += bytes_read;
    }

    xSemaphoreGive(mutex_);
    return bytes_read;
}

bool TimeshiftManager::seek(size_t position) {
    if (!is_open_) return false;

    xSemaphoreTake(mutex_, portMAX_DELAY);

    // Causa #8: Resettiamo lo stato di pre-caricamento dopo un seek.
    last_preload_check_chunk_ = INVALID_CHUNK_ID;

    // Verify that the offset is in a READY chunk
    size_t chunk_id = find_chunk_for_offset(position);
    if (chunk_id == INVALID_CHUNK_ID) {
        xSemaphoreGive(mutex_);
        LOG_WARN("Seek to %u failed: offset not in ready chunks", (unsigned)position);
        return false;
    }

    // Update read offset (next read() will load the correct chunk)
    current_read_offset_ = position;

    xSemaphoreGive(mutex_);
    LOG_INFO("Seek to offset %u (chunk %u)", (unsigned)position, (unsigned)ready_chunks_[chunk_id].id);
    return true;
}

size_t TimeshiftManager::tell() const {
    return current_read_offset_;
}

size_t TimeshiftManager::size() const {
    // For live streams still downloading, return 0 to indicate unknown size
    // This prevents the decoder from thinking we've reached EOF
    if (is_running_) {
        return 0;  // Unknown size for live stream
    }
    // Only when download is complete, return actual size
    return current_recording_offset_;
}

bool TimeshiftManager::is_open() const {
    return is_open_;
}

const char* TimeshiftManager::uri() const {
    return uri_.c_str();
}


size_t TimeshiftManager::buffered_bytes() const {
    // Calculate available bytes in ready chunks
    // Safe to access ready_chunks_.size() without mutex (atomic operation)
    if (ready_chunks_.empty()) {
        return 0;
    }

    // Quick estimation without mutex (to avoid blocking in loop())
    // This is safe because ready_chunks_ only grows (never shrinks during recording)
    size_t num_ready = ready_chunks_.size();
    if (num_ready == 0) return 0;

    // Conservative estimate: num_chunks * CHUNK_SIZE
    // (actual calculation would need mutex, but this is good enough for "ready?" check)
    return num_ready * CHUNK_SIZE;
}

size_t TimeshiftManager::total_downloaded_bytes() const {
    return current_recording_offset_;
}

float TimeshiftManager::buffer_duration_seconds() const {
    // Simple estimation assuming 128kbps
    constexpr float bitrate_kbps = 128.0f;
    return (buffered_bytes() * 8.0f) / (bitrate_kbps * 1024.0f);
}

void TimeshiftManager::download_task_trampoline(void* arg) {
    TimeshiftManager* self = static_cast<TimeshiftManager*>(arg);
    self->download_task_loop();
}

void TimeshiftManager::preloader_task_trampoline(void* arg) {
    static_cast<TimeshiftManager*>(arg)->preloader_task_loop();
}

void TimeshiftManager::preloader_task_loop() {
    LOG_INFO("File preloader task started");
    size_t last_playback_chunk_seen = INVALID_CHUNK_ID;
    bool next_chunk_preloaded = false;

    while (is_running_) {
        vTaskDelay(pdMS_TO_TICKS(100)); // Controlla ogni 100ms

        xSemaphoreTake(mutex_, portMAX_DELAY);

        if (current_playback_chunk_id_ == INVALID_CHUNK_ID || ready_chunks_.empty()) {
            xSemaphoreGive(mutex_);
            continue;
        }

        // Rileva se siamo passati a un nuovo chunk
        if (current_playback_chunk_id_ != last_playback_chunk_seen) {
            last_playback_chunk_seen = current_playback_chunk_id_;
            next_chunk_preloaded = false; // Reset: il nuovo chunk successivo non è ancora stato precaricato
            LOG_DEBUG("Preloader: switched to chunk %u, will preload %u when ready",
                      (unsigned)current_playback_chunk_id_,
                      (unsigned)(current_playback_chunk_id_ + 1));
        }

        // Se il chunk successivo non è ancora stato pre-caricato, controlla se è il momento giusto
        if (!next_chunk_preloaded) {
            size_t next_chunk_id = current_playback_chunk_id_ + 1;

            // Verifica che il chunk successivo esista
            if (next_chunk_id >= ready_chunks_.size()) {
                xSemaphoreGive(mutex_);
                continue; // Chunk non ancora disponibile
            }

            // Calcola il progresso nel chunk corrente
            const auto& current_chunk = ready_chunks_[current_playback_chunk_id_];
            size_t offset_in_chunk = current_read_offset_ - current_chunk.start_offset;

            // Protezione contro offset negativi (può capitare subito dopo uno switch)
            if (current_read_offset_ < current_chunk.start_offset) {
                xSemaphoreGive(mutex_);
                continue;
            }

            float progress = (float)offset_in_chunk / (float)current_chunk.length;

            // Trigger di pre-caricamento (al 50% del chunk corrente)
            if (progress >= 0.50f) {
                if (preload_next_chunk(current_playback_chunk_id_)) {
                    LOG_DEBUG("Preloader task loaded chunk %u", (unsigned)next_chunk_id);
                    next_chunk_preloaded = true;
                }
            }
        }

        xSemaphoreGive(mutex_);
    }
    LOG_INFO("File preloader task terminated");
    vTaskDelete(nullptr);
}

void TimeshiftManager::download_task_loop() {
    LOG_INFO("TimeshiftManager download task started - connecting to %s", uri_.c_str());

    HTTPClient http;
    http.begin(uri_.c_str());
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(10000);
    http.setUserAgent("ESP32-Audio/1.0");

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        LOG_ERROR("HTTP GET failed: %d (%s)", httpCode, http.errorToString(httpCode).c_str());
        is_running_ = false;
        http.end();
        vTaskDelete(nullptr);
        return;
    }

    LOG_INFO("HTTP connected, code: %d - starting download loop", httpCode);
    WiFiClient* stream = http.getStreamPtr();

    uint8_t buf[DOWNLOAD_CHUNK];
    size_t total_downloaded = 0;
    uint32_t last_log_time = millis();

    uint32_t start_wait = millis();
    uint32_t last_data_time = millis();
    const uint32_t STREAM_TIMEOUT = 30000; // 30 seconds without data = timeout

    while (is_running_) {
        // Check pause flag
        if (pause_download_) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int available = stream->available();
        if (available > 0) {
            size_t to_read = std::min(sizeof(buf), (size_t)available);
            int len = stream->readBytes(buf, to_read);

            if (len > 0) {
                last_data_time = millis(); // Reset timeout on successful read

                // Write to recording_buffer_ (circular)
                for (int i = 0; i < len; i++) {
                    recording_buffer_[rec_write_head_] = buf[i];
                    rec_write_head_ = (rec_write_head_ + 1) % BUFFER_SIZE;
                    bytes_in_current_chunk_++;
                }
                total_downloaded += len;

                // --- LOGICA DI FLUSH DECOUPLED ---
                // Controlliamo se è necessario un flush, ma lo eseguiamo *fuori* dal mutex
                // per non bloccare il thread di playback durante la scrittura su SD.
                bool needs_flush = (bytes_in_current_chunk_ >= MIN_CHUNK_FLUSH_SIZE);

                if (needs_flush) {
                    LOG_INFO("Buffer near full (%u bytes), flushing to SD", (unsigned)bytes_in_current_chunk_);
                    if (!flush_recording_chunk()) {
                        LOG_ERROR("Failed to flush recording chunk");
                        // Potremmo voler gestire l'errore in modo più robusto qui
                    }
                }

                xSemaphoreTake(mutex_, portMAX_DELAY);
                // Log progress every 5 seconds
                if (millis() - last_log_time > 5000) {
                    LOG_INFO("Recording: %u KB total, %u bytes in current chunk, %u ready chunks",
                             (unsigned)(total_downloaded / 1024),
                             (unsigned)bytes_in_current_chunk_,
                             (unsigned)ready_chunks_.size());
                    last_log_time = millis();
                }
                xSemaphoreGive(mutex_);
            }
        } else {
            // No data available, check for timeout
            if (millis() - last_data_time > STREAM_TIMEOUT) {
                LOG_WARN("Stream timeout (no data for %u sec), reconnecting...", STREAM_TIMEOUT / 1000);
                http.end();
                vTaskDelay(pdMS_TO_TICKS(1000));

                http.begin(uri_.c_str());
                http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
                http.setTimeout(10000);
                httpCode = http.GET();

                if (httpCode != HTTP_CODE_OK) {
                    LOG_ERROR("Reconnection failed: %d", httpCode);
                    break;
                }
                stream = http.getStreamPtr();
                last_data_time = millis();
                LOG_INFO("Reconnected successfully");
            }

            vTaskDelay(pdMS_TO_TICKS(50)); // Wait a bit longer when no data
        }
    }

    LOG_INFO("Download task ending - total downloaded: %u KB", (unsigned)(total_downloaded / 1024));
    http.end();
    vTaskDelete(nullptr);
}

// ========== RECORDING SIDE METHODS ==========

bool TimeshiftManager::flush_recording_chunk() {
    // Create ChunkInfo for PENDING chunk
    ChunkInfo chunk;
    chunk.id = next_chunk_id_++;
    chunk.start_offset = current_recording_offset_;
    chunk.length = bytes_in_current_chunk_;
    chunk.end_offset = current_recording_offset_ + bytes_in_current_chunk_;
    chunk.state = ChunkState::PENDING;
    chunk.crc32 = 0; // TODO: calculate CRC if needed
    chunk.psram_ptr = nullptr;

    // Write to storage based on mode (without holding mutex)
    bool write_success = false;
    if (storage_mode_ == StorageMode::SD_CARD) {
        chunk.filename = "/timeshift/pending_" + std::to_string(chunk.id) + ".bin";
        write_success = write_chunk_to_sd(chunk);
    } else {
        chunk.filename = "";  // Not used in PSRAM mode
        write_success = write_chunk_to_psram(chunk);
    }

    if (!write_success) {
        LOG_ERROR("Failed to write chunk %u to storage", chunk.id);
        return false;
    }

    // *** MODIFICA CHIAVE: Acquisiamo il mutex SOLO ora, per promuovere il chunk ***
    // Questa operazione è velocissima (rename + push_back) e non bloccherà il playback.
    xSemaphoreTake(mutex_, portMAX_DELAY);

    // Aggiorna gli offset di registrazione *dentro* il lock per consistenza
    current_recording_offset_ += bytes_in_current_chunk_;
    bytes_in_current_chunk_ = 0;
    // rec_write_head_ non viene resettato, il buffer è circolare

    // Validate and promote to READY
    if (validate_chunk(chunk)) {
        promote_chunk_to_ready(chunk);
    } else {
        LOG_ERROR("Chunk %u validation failed", chunk.id);
        SD_MMC.remove(chunk.filename.c_str());
        return false;
    }

    // Cleanup old chunks (chiamato dopo ogni flush di chunk)
    LOG_DEBUG("Calling cleanup_old_chunks() after flushing chunk %u", chunk.id);
    cleanup_old_chunks();

    // Rilasciamo il mutex
    xSemaphoreGive(mutex_);

    return true;
}

bool TimeshiftManager::write_chunk_to_sd(ChunkInfo& chunk) {
    File file = SD_MMC.open(chunk.filename.c_str(), FILE_WRITE);
    if (!file) {
        LOG_ERROR("Failed to open chunk file for write: %s", chunk.filename.c_str());
        return false;
    }

    // The recording_buffer_ might have wrapped around, so we need to handle circular buffer
    // Calculate start position: current position minus accumulated bytes
    size_t start_pos = 0;
    if (rec_write_head_ >= chunk.length) {
        // Data is contiguous: [start_pos ... rec_write_head_)
        start_pos = rec_write_head_ - chunk.length;
        size_t written = file.write(recording_buffer_ + start_pos, chunk.length);
        file.close();

        if (written != chunk.length) {
            LOG_ERROR("Chunk write mismatch: expected %u, wrote %u", chunk.length, written);
            SD_MMC.remove(chunk.filename.c_str());
            return false;
        }
    } else {
        // Data wraps around: [BUFFER_SIZE - remainder ... BUFFER_SIZE) + [0 ... rec_write_head_)
        size_t remainder = chunk.length - rec_write_head_;
        start_pos = BUFFER_SIZE - remainder;

        // Write first part (from start_pos to end of buffer)
        size_t written1 = file.write(recording_buffer_ + start_pos, remainder);

        // Write second part (from 0 to rec_write_head_)
        size_t written2 = file.write(recording_buffer_, rec_write_head_);

        file.close();

        if (written1 + written2 != chunk.length) {
            LOG_ERROR("Chunk write mismatch: expected %u, wrote %u", chunk.length, (unsigned)(written1 + written2));
            SD_MMC.remove(chunk.filename.c_str());
            return false;
        }
    }

    LOG_DEBUG("Wrote chunk %u: %u KB to %s", chunk.id, chunk.length / 1024, chunk.filename.c_str());
    return true;
}

bool TimeshiftManager::write_chunk_to_psram(ChunkInfo& chunk) {
    // Allocate PSRAM chunk from pool (circular)
    chunk.psram_ptr = allocate_psram_chunk();
    if (!chunk.psram_ptr) {
        LOG_ERROR("Failed to allocate PSRAM chunk");
        return false;
    }

    // Copy data from circular recording buffer to PSRAM chunk
    size_t start_pos = 0;
    if (rec_write_head_ >= chunk.length) {
        // Data is contiguous: [start_pos ... rec_write_head_)
        start_pos = rec_write_head_ - chunk.length;
        memcpy(chunk.psram_ptr, recording_buffer_ + start_pos, chunk.length);
    } else {
        // Data wraps around: [BUFFER_SIZE - remainder ... BUFFER_SIZE) + [0 ... rec_write_head_)
        size_t remainder = chunk.length - rec_write_head_;
        start_pos = BUFFER_SIZE - remainder;

        // Copy first part (from start_pos to end of buffer)
        memcpy(chunk.psram_ptr, recording_buffer_ + start_pos, remainder);

        // Copy second part (from 0 to rec_write_head_)
        memcpy(chunk.psram_ptr + remainder, recording_buffer_, rec_write_head_);
    }

    LOG_DEBUG("Wrote chunk %u: %u KB to PSRAM (pool index %u)",
              chunk.id, chunk.length / 1024, (unsigned)(chunk.id % MAX_PSRAM_CHUNKS));
    return true;
}

bool TimeshiftManager::validate_chunk(ChunkInfo& chunk) {
    if (storage_mode_ == StorageMode::SD_CARD) {
        // SD mode: check file exists and size matches
        File file = SD_MMC.open(chunk.filename.c_str(), FILE_READ);
        if (!file) {
            LOG_ERROR("Validation failed: cannot open %s", chunk.filename.c_str());
            return false;
        }

        size_t file_size = file.size();
        file.close();

        if (file_size != chunk.length) {
            LOG_ERROR("Validation failed: size mismatch (%u vs %u)", file_size, chunk.length);
            return false;
        }
    } else {
        // PSRAM mode: check pointer is valid
        if (!chunk.psram_ptr) {
            LOG_ERROR("Validation failed: null PSRAM pointer for chunk %u", chunk.id);
            return false;
        }
        // Size is always correct in PSRAM mode (direct copy)
    }

    // TODO: Add CRC32 validation if needed
    return true;
}

bool TimeshiftManager::calculate_chunk_duration(const ChunkInfo& chunk,
                                                 uint32_t& out_frames,
                                                 uint32_t& out_duration_ms)
{
    uint8_t header[4];
    uint32_t total_samples = 0;
    uint32_t sample_rate = 44100;  // Default, will be updated from first frame
    size_t data_pos = 0;  // Current position in chunk data

    // Helper lambda to read data from either SD or PSRAM
    auto read_bytes = [&](uint8_t* buffer, size_t len) -> size_t {
        if (storage_mode_ == StorageMode::PSRAM_ONLY) {
            if (data_pos + len > chunk.length) {
                len = chunk.length - data_pos;
            }
            if (len == 0) return 0;
            memcpy(buffer, chunk.psram_ptr + data_pos, len);
            data_pos += len;
            return len;
        }
        return 0; // Will be handled by file read in SD mode
    };

    File file;
    if (storage_mode_ == StorageMode::SD_CARD) {
        file = SD_MMC.open(chunk.filename.c_str(), FILE_READ);
        if (!file) {
            LOG_ERROR("Cannot open chunk for duration calculation: %s", chunk.filename.c_str());
            return false;
        }
    } else {
        // PSRAM mode: use psram_ptr
        if (!chunk.psram_ptr) {
            LOG_ERROR("Cannot calculate duration: null PSRAM pointer");
            return false;
        }
    }

    // Scan all MP3 frames in the chunk
    while (true) {
        size_t bytes_read;
        if (storage_mode_ == StorageMode::SD_CARD) {
            if (!file.available()) break;
            bytes_read = file.read(header, 4);
        } else {
            bytes_read = read_bytes(header, 4);
        }

        if (bytes_read != 4) break;

        // Check sync word (0xFFE or 0xFFF at start)
        if ((header[0] != 0xFF) || ((header[1] & 0xE0) != 0xE0)) {
            continue;  // Not a frame header, advance by 1 byte and retry
        }

        // Parse MP3 frame header
        uint8_t b1 = header[1];
        uint8_t b2 = header[2];

        int version_id = (b1 >> 3) & 0x03;      // MPEG version
        int layer_idx = (b1 >> 1) & 0x03;       // Layer
        int bitrate_idx = (b2 >> 4) & 0x0F;     // Bitrate index
        int sr_idx = (b2 >> 2) & 0x03;          // Sample rate index
        int padding = (b2 >> 1) & 0x01;         // Padding bit

        // Validate header fields
        if (layer_idx == 0 || bitrate_idx == 0x0F || sr_idx == 0x03) {
            continue;  // Invalid header, skip
        }

        // Bitrate tables (kbps)
        static const uint16_t bitrate_table[2][16] = {
            {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},  // MPEG1
            {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}       // MPEG2/2.5
        };

        // Sample rate tables
        static const uint32_t sr_table[3][3] = {
            {44100, 48000, 32000},  // MPEG1
            {22050, 24000, 16000},  // MPEG2
            {11025, 12000, 8000}    // MPEG2.5
        };

        int version_row = (version_id == 0x03) ? 0 : 1;  // MPEG1 vs MPEG2/2.5
        uint32_t bitrate_kbps = bitrate_table[version_row][bitrate_idx];

        int sr_row = (version_id == 0x03) ? 0 : (version_id == 0x02) ? 1 : 2;
        sample_rate = sr_table[sr_row][sr_idx];

        if (bitrate_kbps == 0 || sample_rate == 0) {
            continue;  // Invalid values, skip
        }

        // Calculate frame size: FrameSize = 144 * BitRate / SampleRate + Padding
        int frame_size = (144 * bitrate_kbps * 1000) / sample_rate + padding;

        if (frame_size <= 0 || frame_size > 4096) {
            continue;  // Unreasonable frame size, skip
        }

        // Each MP3 frame contains 1152 samples per channel (Layer 3)
        uint32_t samples_per_frame = 1152;
        if (layer_idx == 3) samples_per_frame = 384;      // Layer 1
        else if (layer_idx == 2) samples_per_frame = 1152; // Layer 2
        else samples_per_frame = ((version_id == 0x03) ? 1152 : 576); // Layer 3

        total_samples += samples_per_frame;

        // Skip to next frame
        if (storage_mode_ == StorageMode::SD_CARD) {
            size_t current_pos = file.position();
            if (current_pos + frame_size - 4 <= file.size()) {
                file.seek(current_pos + frame_size - 4);
            } else {
                break;  // Would go beyond file end
            }
        } else {
            // PSRAM mode: advance data_pos
            size_t skip_bytes = frame_size - 4;
            if (data_pos + skip_bytes > chunk.length) {
                break;  // Would go beyond chunk end
            }
            data_pos += skip_bytes;
        }
    }

    if (storage_mode_ == StorageMode::SD_CARD && file) {
        file.close();
    }

    if (total_samples == 0) {
        LOG_WARN("Chunk %u: no valid MP3 frames found", chunk.id);
        return false;
    }

    // Calculate duration in milliseconds
    out_frames = total_samples;
    out_duration_ms = (total_samples * 1000) / sample_rate;

    LOG_DEBUG("Chunk %u: %u samples, %u ms @ %u Hz",
              chunk.id, out_frames, out_duration_ms, sample_rate);

    return true;
}

void TimeshiftManager::promote_chunk_to_ready(ChunkInfo chunk) {
    if (storage_mode_ == StorageMode::SD_CARD) {
        // Rename file from pending to ready
        std::string ready_filename = "/timeshift/ready_" + std::to_string(chunk.id) + ".bin";

        // Remove destination file if it exists (from previous session)
        if (SD_MMC.exists(ready_filename.c_str())) {
            SD_MMC.remove(ready_filename.c_str());
            LOG_DEBUG("Removed existing ready file: %s", ready_filename.c_str());
        }

        if (!SD_MMC.rename(chunk.filename.c_str(), ready_filename.c_str())) {
            LOG_ERROR("Failed to rename chunk %u from pending to ready", chunk.id);
            SD_MMC.remove(chunk.filename.c_str());
            return;
        }
        chunk.filename = ready_filename;
    }

    // Mark as READY (for both SD and PSRAM modes)
    chunk.state = ChunkState::READY;

    // Calculate chunk duration
    uint32_t total_frames = 0;
    uint32_t duration_ms = 0;

    if (calculate_chunk_duration(chunk, total_frames, duration_ms)) {
        chunk.total_frames = total_frames;
        chunk.duration_ms = duration_ms;
        chunk.start_time_ms = cumulative_time_ms_;

        cumulative_time_ms_ += duration_ms;  // Update cumulative time

        LOG_INFO("Chunk %u promoted to READY (%u KB, offset %u-%u, %u ms, %u frames)",
                 chunk.id, chunk.length / 1024,
                 (unsigned)chunk.start_offset, (unsigned)chunk.end_offset,
                 duration_ms, total_frames);
    } else {
        LOG_WARN("Chunk %u promoted to READY without duration info (%u KB, offset %u-%u)",
                 chunk.id, chunk.length / 1024,
                 (unsigned)chunk.start_offset, (unsigned)chunk.end_offset);
    }

    // Add to ready_chunks_ (already ordered by ID)
    ready_chunks_.push_back(chunk);
}

void TimeshiftManager::cleanup_old_chunks() {
    // Remove chunks beyond the timeshift window
    LOG_DEBUG("=== CLEANUP START ===");
    LOG_DEBUG("Current recording offset: %u MB (%u bytes)",
              (unsigned)(current_recording_offset_ / (1024 * 1024)),
              (unsigned)current_recording_offset_);
    LOG_DEBUG("MAX_TS_WINDOW: %u MB (%u bytes)",
              (unsigned)(MAX_TS_WINDOW / (1024 * 1024)),
              (unsigned)MAX_TS_WINDOW);
    LOG_DEBUG("Total ready chunks: %u", (unsigned)ready_chunks_.size());

    if (ready_chunks_.empty()) {
        LOG_DEBUG("No chunks to cleanup (ready_chunks_ is empty)");
        LOG_DEBUG("=== CLEANUP END (nothing to do) ===");
        return;
    }

    size_t removed_count = 0;
    size_t total_removed_bytes = 0;

    while (!ready_chunks_.empty()) {
        const ChunkInfo& oldest = ready_chunks_.front();
        size_t age_bytes = current_recording_offset_ - oldest.end_offset;
        size_t age_mb = age_bytes / (1024 * 1024);

        LOG_DEBUG("Checking oldest chunk %u: end_offset=%u MB, age=%u MB (%u bytes)",
                  oldest.id,
                  (unsigned)(oldest.end_offset / (1024 * 1024)),
                  (unsigned)age_mb,
                  (unsigned)age_bytes);

        if (age_bytes > MAX_TS_WINDOW) {
            LOG_INFO("CLEANUP: Removing old chunk %u (age: %u MB > limit: %u MB)",
                     oldest.id,
                     (unsigned)age_mb,
                     (unsigned)(MAX_TS_WINDOW / (1024 * 1024)));

            if (storage_mode_ == StorageMode::SD_CARD) {
                LOG_INFO("   File: %s, Size: %u KB",
                         oldest.filename.c_str(),
                         (unsigned)(oldest.length / 1024));

                bool removed = SD_MMC.remove(oldest.filename.c_str());
                if (removed) {
                    LOG_DEBUG("   File deleted successfully");
                    removed_count++;
                    total_removed_bytes += oldest.length;
                } else {
                    LOG_ERROR("   Failed to delete file: %s", oldest.filename.c_str());
                }
            } else {
                // PSRAM mode: chunk slot will be reused automatically
                LOG_DEBUG("   PSRAM chunk slot %u freed (will be reused)",
                          (unsigned)(oldest.id % MAX_PSRAM_CHUNKS));
                removed_count++;
                total_removed_bytes += oldest.length;
            }

            ready_chunks_.erase(ready_chunks_.begin());
        } else {
            LOG_DEBUG("Oldest chunk %u is still within window (age: %u MB <= limit: %u MB), stopping cleanup",
                      oldest.id,
                      (unsigned)age_mb,
                      (unsigned)(MAX_TS_WINDOW / (1024 * 1024)));
            break; // Chunks are ordered, so we can stop
        }
    }

    if (removed_count > 0) {
        LOG_INFO("CLEANUP SUMMARY: Removed %u chunks, freed %u MB",
                 (unsigned)removed_count,
                 (unsigned)(total_removed_bytes / (1024 * 1024)));
    } else {
        LOG_DEBUG("No chunks removed (all within window)");
    }

    LOG_DEBUG("Remaining ready chunks: %u", (unsigned)ready_chunks_.size());
    LOG_DEBUG("=== CLEANUP END ===");
}

bool TimeshiftManager::preload_next_chunk(size_t current_chunk_id) {
    size_t next_chunk_id = current_chunk_id + 1;

    // Il preloader task non attende, controlla solo se il chunk è disponibile
    if (next_chunk_id >= ready_chunks_.size()) {
        return false; // Non è ancora pronto, riproverà al prossimo ciclo
    }

    const ChunkInfo& next_chunk = ready_chunks_[next_chunk_id];
    if (next_chunk.state != ChunkState::READY) { // Controllo di sicurezza
        LOG_WARN("Preload failed: chunk %u is not in READY state", (unsigned)next_chunk_id);
        return false;
    }

    // Il buffer di playback è 256KB. Il chunk corrente è a [0-128KB].
    // Pre-carichiamo il successivo a [128KB-256KB].
    if (storage_mode_ == StorageMode::SD_CARD) {
        File file = SD_MMC.open(next_chunk.filename.c_str(), FILE_READ);
        if (!file) {
            LOG_ERROR("Preload failed: cannot open %s", next_chunk.filename.c_str());
            return false;
        }

        // Carica nella seconda metà del buffer
        size_t read = file.read(playback_buffer_ + CHUNK_SIZE, next_chunk.length);
        file.close();

        if (read != next_chunk.length) {
            LOG_ERROR("Preload read mismatch: expected %u, got %u", next_chunk.length, read);
            return false;
        }
    } else {
        // PSRAM mode: direct copy from PSRAM pool
        if (!next_chunk.psram_ptr) {
            LOG_ERROR("Preload failed: null PSRAM pointer for chunk %u", next_chunk.id);
            return false;
        }
        memcpy(playback_buffer_ + CHUNK_SIZE, next_chunk.psram_ptr, next_chunk.length);
    }

    LOG_DEBUG("Preloaded chunk %u (%u KB) at buffer offset %u",
              next_chunk.id, next_chunk.length / 1024, (unsigned)CHUNK_SIZE);

    return true;
}

// ========== PLAYBACK SIDE METHODS ==========

size_t TimeshiftManager::find_chunk_for_offset(size_t offset) {
    // --- RICERCA ROBUSTA ---
    // Cerca il chunk che CONTIENE l'offset o il chunk SUCCESSIVO se l'offset
    // si trova in un piccolo "buco" tra due chunk.
    if (ready_chunks_.empty()) {
        return INVALID_CHUNK_ID;
    }

    // Binary search per trovare il primo chunk il cui end_offset è > offset
    size_t low = 0, high = ready_chunks_.size();
    size_t best_match = INVALID_CHUNK_ID;

    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (ready_chunks_[mid].start_offset > offset) {
            high = mid;
        } else {
            best_match = mid;
            low = mid + 1;
        }
    }

    if (best_match != INVALID_CHUNK_ID) {
        const auto& chunk = ready_chunks_[best_match];
        // Se l'offset è nel chunk o in un piccolo gap prima del successivo, è valido.
        // Il "gap" massimo tollerato è di 4KB, più che sufficiente.
        if (offset <= chunk.end_offset || (best_match + 1 < ready_chunks_.size() && offset < ready_chunks_[best_match + 1].start_offset + 4096)) {
            return best_match;
        }
    }

    // Se arriviamo qui, l'offset è veramente fuori range. Logghiamo per debug.
    LOG_ERROR("STUTTER DETECTED: No chunk found for offset %u. Last chunk ends at %u.",
              (unsigned)offset, (unsigned)ready_chunks_.back().end_offset);
    return INVALID_CHUNK_ID;
}

bool TimeshiftManager::load_chunk_to_playback(size_t chunk_id) {
    if (chunk_id >= ready_chunks_.size()) {
        LOG_ERROR("Invalid chunk ID: %u (max: %u)", chunk_id, ready_chunks_.size());
        return false;
    }

    const ChunkInfo& chunk = ready_chunks_[chunk_id];
    if (chunk.state != ChunkState::READY) {
        LOG_ERROR("Chunk %u is not READY (state: %d)", chunk.id, (int)chunk.state);
        return false;
    }

    // Load chunk data based on storage mode
    if (storage_mode_ == StorageMode::SD_CARD) {
        // Open chunk file
        File file = SD_MMC.open(chunk.filename.c_str(), FILE_READ);
        if (!file) {
            LOG_ERROR("Failed to open chunk for playback: %s", chunk.filename.c_str());
            return false;
        }

        // Read entire chunk into playback_buffer_
        size_t read = file.read(playback_buffer_, chunk.length);
        file.close();

        if (read != chunk.length) {
            LOG_ERROR("Chunk read mismatch: expected %u, got %u", chunk.length, read);
            return false;
        }
    } else {
        // PSRAM mode: direct copy from PSRAM pool
        if (!chunk.psram_ptr) {
            LOG_ERROR("Failed to load chunk %u: null PSRAM pointer", chunk.id);
            return false;
        }
        memcpy(playback_buffer_, chunk.psram_ptr, chunk.length);
    }

    // Update playback state
    current_playback_chunk_id_ = chunk_id;
    playback_chunk_loaded_size_ = chunk.length;

    // Log with temporal info for better user understanding
    uint32_t start_min = chunk.start_time_ms / 60000;
    uint32_t start_sec = (chunk.start_time_ms / 1000) % 60;
    uint32_t end_ms = chunk.start_time_ms + chunk.duration_ms;
    uint32_t end_min = end_ms / 60000;
    uint32_t end_sec = (end_ms / 1000) % 60;

    LOG_INFO("→ Loaded chunk %u (%u KB) [%02u:%02u - %02u:%02u]",
             chunk.id, chunk.length / 1024,
             start_min, start_sec, end_min, end_sec);

    return true;
}

size_t TimeshiftManager::read_from_playback_buffer(size_t offset, void* buffer, size_t size) {
    // Find chunk containing this offset
    size_t chunk_id = find_chunk_for_offset(offset);
    if (chunk_id == INVALID_CHUNK_ID) {
        LOG_WARN("No chunk found for offset %u", (unsigned)offset);
        return 0;
    }

    // Se il chunk richiesto è quello successivo, esegui lo switch "seamless"
    if (chunk_id > current_playback_chunk_id_) {
        // Causa #2: Usiamo la dimensione reale del chunk pre-caricato per memmove.
        const ChunkInfo& preloaded_chunk_info = ready_chunks_[chunk_id];
        size_t preloaded_size = preloaded_chunk_info.length;
        // Sposta il chunk pre-caricato (che si trova nella seconda metà del buffer) all'inizio.
        memmove(playback_buffer_, playback_buffer_ + CHUNK_SIZE, preloaded_size);

        current_playback_chunk_id_ = chunk_id;
        playback_chunk_loaded_size_ = ready_chunks_[chunk_id].length;
        last_preload_check_chunk_ = INVALID_CHUNK_ID; // Resetta il tracking per il nuovo chunk
        LOG_DEBUG("Switching to preloaded chunk %u (seamless)", (unsigned)chunk_id);

        // Triggera immediatamente il preload del prossimo chunk
        // (il preloader task lo caricherà non appena arriveremo al 50% di questo chunk)
        if (chunk_id + 1 < ready_chunks_.size()) {
            // Potremmo anche triggerare un preload immediato qui se vogliamo essere più aggressivi
            // Per ora lasciamo che il preloader task lo gestisca al 50% del chunk corrente
        }

    } else if (current_playback_chunk_id_ != chunk_id) {
        // Seek o situazione anomala: carica il chunk richiesto da SD (potrebbe causare stutter)
        LOG_WARN("Chunk %u not preloaded, loading now (may cause stutter)", (unsigned)chunk_id);

        // Attiva la pausa automatica per buffering se il callback è registrato
        if (auto_pause_callback_ && !is_auto_paused_) {
            LOG_INFO("Auto-pausing playback for buffering...");
            is_auto_paused_ = true;
            auto_pause_callback_(true);  // true = pause
        }

        if (!load_chunk_to_playback(chunk_id)) {
            LOG_ERROR("Failed to load chunk %u for playback", (unsigned)chunk_id);
            return 0;
        }

        // Attendi il margine configurato per permettere al preloader di caricare i chunk successivi
        if (auto_pause_callback_ && is_auto_paused_) {
            // Se il margine è 0, riprendi immediatamente senza attese
            if (auto_pause_delay_ms_ == 0 && auto_pause_min_chunks_ == 0) {
                LOG_INFO("Chunk loaded, resuming immediately (no buffer margin configured)");
                is_auto_paused_ = false;
                xSemaphoreGive(mutex_);
                auto_pause_callback_(false);  // false = resume
                xSemaphoreTake(mutex_, portMAX_DELAY);
            } else {
                LOG_INFO("Chunk loaded, waiting for buffer margin before resuming...");

                // Rilascia il mutex durante l'attesa per non bloccare il download task
                if (auto_pause_delay_ms_ > 0) {
                    xSemaphoreGive(mutex_);
                    vTaskDelay(pdMS_TO_TICKS(auto_pause_delay_ms_));
                    xSemaphoreTake(mutex_, portMAX_DELAY);
                }

                // Verifica che ci siano abbastanza chunk pronti prima di riprendere
                if (auto_pause_min_chunks_ > 0) {
                    const uint32_t MAX_WAIT_MS = 3000;  // Massimo 3 secondi di attesa aggiuntiva
                    uint32_t start_wait = millis();
                    size_t target_chunks = chunk_id + auto_pause_min_chunks_;

                    while (ready_chunks_.size() < target_chunks && (millis() - start_wait) < MAX_WAIT_MS) {
                        xSemaphoreGive(mutex_);
                        vTaskDelay(pdMS_TO_TICKS(100));
                        xSemaphoreTake(mutex_, portMAX_DELAY);
                    }
                }

                LOG_INFO("Buffer ready (%u chunks available), resuming playback...", (unsigned)ready_chunks_.size());
                is_auto_paused_ = false;

                // Riprendi la riproduzione DOPO aver ri-acquisito il mutex
                xSemaphoreGive(mutex_);
                auto_pause_callback_(false);  // false = resume
                xSemaphoreTake(mutex_, portMAX_DELAY);
            }
        }
    }

    // Calculate offset relative to chunk
    const ChunkInfo& chunk = ready_chunks_[chunk_id];
    size_t chunk_offset = offset - chunk.start_offset;
    size_t available = playback_chunk_loaded_size_ - chunk_offset;
    size_t to_read = std::min(size, available);

    // Copia i dati dal buffer di playback
    memcpy(buffer, playback_buffer_ + chunk_offset, to_read);

    return to_read;
}

// ========== TEMPORAL SEEK METHODS ==========

size_t TimeshiftManager::seek_to_time(uint32_t target_ms) {
    xSemaphoreTake(mutex_, portMAX_DELAY);

    if (ready_chunks_.empty()) {
        xSemaphoreGive(mutex_);
        LOG_WARN("Seek to time failed: no ready chunks available");
        return SIZE_MAX;  // Invalid offset
    }

    // Search for the chunk containing the target timestamp
    for (size_t i = 0; i < ready_chunks_.size(); i++) {
        const ChunkInfo& chunk = ready_chunks_[i];
        uint32_t chunk_end_ms = chunk.start_time_ms + chunk.duration_ms;

        if (target_ms >= chunk.start_time_ms && target_ms < chunk_end_ms) {
            // Found the chunk! Calculate relative offset within chunk
            uint32_t offset_ms = target_ms - chunk.start_time_ms;
            float progress = (float)offset_ms / (float)chunk.duration_ms;

            // Estimate byte offset (linear interpolation)
            size_t byte_offset_in_chunk = (size_t)(chunk.length * progress);
            size_t global_offset = chunk.start_offset + byte_offset_in_chunk;

            xSemaphoreGive(mutex_);

            LOG_INFO("Seek to %u ms → chunk %u, byte offset %u (progress %.2f%%)",
                     target_ms, chunk.id, (unsigned)global_offset, progress * 100.0f);

            return global_offset;
        }
    }

    // If target is beyond available time, seek to last available position
    const ChunkInfo& last_chunk = ready_chunks_.back();
    size_t last_offset = last_chunk.end_offset - 1;

    xSemaphoreGive(mutex_);

    LOG_WARN("Seek to %u ms beyond available time (%u ms), seeking to end",
             target_ms, last_chunk.start_time_ms + last_chunk.duration_ms);

    return last_offset;
}

uint32_t TimeshiftManager::total_duration_ms() const {
    if (ready_chunks_.empty()) return 0;

    // Sum all chunk durations
    uint32_t total = 0;
    for (const auto& chunk : ready_chunks_) {
        total += chunk.duration_ms;
    }

    return total;
}

uint32_t TimeshiftManager::current_position_ms() const {
    if (ready_chunks_.empty()) return 0;

    // Find the chunk containing current_read_offset_
    for (const auto& chunk : ready_chunks_) {
        if (current_read_offset_ >= chunk.start_offset &&
            current_read_offset_ < chunk.end_offset) {

            // Calculate relative offset within chunk
            size_t offset_in_chunk = current_read_offset_ - chunk.start_offset;
            float progress = (float)offset_in_chunk / (float)chunk.length;

            // Calculate time within chunk
            uint32_t time_in_chunk = (uint32_t)(chunk.duration_ms * progress);

            return chunk.start_time_ms + time_in_chunk;
        }
    }

    // If not found in any chunk, return 0
    return 0;
}

// ========== PAUSE/RESUME METHODS ==========

void TimeshiftManager::pause_recording() {
    pause_download_ = true;
    LOG_INFO("Recording paused");
}

void TimeshiftManager::resume_recording() {
    pause_download_ = false;
    LOG_INFO("Recording resumed");
}

// ========== STORAGE BACKEND HELPERS ==========

bool TimeshiftManager::init_psram_pool() {
    if (psram_chunk_pool_) {
        LOG_WARN("PSRAM pool already allocated");
        return true;
    }

    // Allocate pool for MAX_PSRAM_CHUNKS chunks
    psram_pool_size_ = MAX_PSRAM_CHUNKS * CHUNK_SIZE;

    // Try to allocate in PSRAM first (heap_caps_malloc with MALLOC_CAP_SPIRAM)
    psram_chunk_pool_ = (uint8_t*)heap_caps_malloc(psram_pool_size_, MALLOC_CAP_SPIRAM);

    if (!psram_chunk_pool_) {
        LOG_ERROR("Failed to allocate %u KB in PSRAM", psram_pool_size_ / 1024);
        psram_pool_size_ = 0;
        return false;
    }

    LOG_INFO("PSRAM pool allocated: %u KB (%u chunks x %u KB)",
             psram_pool_size_ / 1024, MAX_PSRAM_CHUNKS, CHUNK_SIZE / 1024);

    return true;
}

void TimeshiftManager::free_psram_pool() {
    if (psram_chunk_pool_) {
        heap_caps_free(psram_chunk_pool_);
        psram_chunk_pool_ = nullptr;
        psram_pool_size_ = 0;
        LOG_DEBUG("PSRAM pool freed");
    }
}

uint8_t* TimeshiftManager::allocate_psram_chunk() {
    if (!psram_chunk_pool_) {
        LOG_ERROR("PSRAM pool not initialized");
        return nullptr;
    }

    // Calculate circular index based on next_chunk_id_
    size_t pool_index = next_chunk_id_ % MAX_PSRAM_CHUNKS;
    uint8_t* chunk_ptr = psram_chunk_pool_ + (pool_index * CHUNK_SIZE);

    LOG_DEBUG("Allocated PSRAM chunk at pool index %u (chunk ID %u)",
              (unsigned)pool_index, next_chunk_id_);

    return chunk_ptr;
}

void TimeshiftManager::free_chunk_storage(ChunkInfo& chunk) {
    if (storage_mode_ == StorageMode::SD_CARD) {
        if (!chunk.filename.empty()) {
            SD_MMC.remove(chunk.filename.c_str());
            LOG_DEBUG("Removed SD file: %s", chunk.filename.c_str());
        }
    } else {
        // PSRAM mode: nothing to free (pool is reused circularly)
        LOG_DEBUG("PSRAM chunk %u freed (slot reusable)", chunk.id);
    }
}
