#include "timeshift_manager.h"
#include "logger.h"
#include "drivers/sd_card_driver.h"  // For SD card access
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>  // For PSRAM allocation
#include "mp3_seek_table.h"

#include <algorithm>
#include <cstdlib>

// ========== ADAPTIVE BUFFER CONFIGURATION ==========
// Target chunk duration: 5-10 seconds of audio for optimal balance
constexpr uint32_t TARGET_CHUNK_DURATION_SEC = 7;  // 7 seconds per chunk (balanced)
constexpr uint32_t MIN_CHUNK_DURATION_SEC = 4;     // Minimum 4 seconds
constexpr uint32_t MAX_CHUNK_DURATION_SEC = 12;    // Maximum 12 seconds

// Cleanup window
constexpr size_t MAX_TS_WINDOW = 1024 * 1024 * 2; // 2 MB max window

// Default bitrate assumption (will be auto-detected from stream)
constexpr uint32_t DEFAULT_BITRATE_KBPS = 128;

TimeshiftManager::TimeshiftManager() {
    mutex_ = xSemaphoreCreateMutex();
    is_auto_paused_ = false;
    // Initialize with default bitrate, will be adapted after first chunk
    calculate_adaptive_sizes(DEFAULT_BITRATE_KBPS);
}

TimeshiftManager::~TimeshiftManager() {
    stop();
    close();
    if (mutex_) vSemaphoreDelete(mutex_);
    if (recording_buffer_) free(recording_buffer_);
    if (playback_buffer_) free(playback_buffer_);
    free_psram_pool();
}

// ========== ADAPTIVE BITRATE DETECTION ==========

void TimeshiftManager::calculate_adaptive_sizes(uint32_t bitrate_kbps) {
    // FORMULA: chunk_size = (bitrate_kbps * 1000 / 8) * target_duration_sec
    // Ensures chunks contain TARGET_CHUNK_DURATION_SEC seconds of audio

    uint32_t target_chunk_bytes = (bitrate_kbps * 1000 / 8) * TARGET_CHUNK_DURATION_SEC;

    // Clamp to reasonable limits (16KB - 512KB)
    const size_t MIN_CHUNK_SIZE = 16 * 1024;   // 16KB minimum
    const size_t MAX_CHUNK_SIZE = 512 * 1024;  // 512KB maximum

    dynamic_chunk_size_ = std::max(MIN_CHUNK_SIZE,
                          std::min(MAX_CHUNK_SIZE, (size_t)target_chunk_bytes));

    // Recording buffer: 1.5x chunk size (50% safety margin)
    dynamic_buffer_size_ = dynamic_chunk_size_ + (dynamic_chunk_size_ / 2);

    // Playback buffer: 3x chunk size (current + next + safety)
    dynamic_playback_buffer_size_ = dynamic_chunk_size_ * 3;

    // Flush threshold: 80% of chunk size
    dynamic_min_flush_size_ = (dynamic_chunk_size_ * 4) / 5;

    // Download chunk: proportional to bitrate (faster streams = larger reads)
    if (bitrate_kbps <= 64) {
        dynamic_download_chunk_ = 2048;   // 2KB for low bitrate
    } else if (bitrate_kbps <= 128) {
        dynamic_download_chunk_ = 4096;   // 4KB for medium
    } else {
        dynamic_download_chunk_ = 8192;   // 8KB for high bitrate
    }

    detected_bitrate_kbps_ = bitrate_kbps;

    LOG_INFO("Adaptive sizing for %u kbps: chunk=%u KB, buffer=%u KB, playback=%u KB, download=%u B",
             bitrate_kbps,
             (unsigned)(dynamic_chunk_size_ / 1024),
             (unsigned)(dynamic_buffer_size_ / 1024),
             (unsigned)(dynamic_playback_buffer_size_ / 1024),
             (unsigned)dynamic_download_chunk_);
}

void TimeshiftManager::apply_bitrate_measurement(uint32_t measured_kbps) {
    if (measured_kbps == 0) {
        return;
    }

    bitrate_history_.push_back(measured_kbps);
    while (bitrate_history_.size() > BITRATE_HISTORY_SIZE) {
        bitrate_history_.pop_front();
    }

    if (bitrate_history_.size() < 2) {
        return;  // Need at least two samples for averaging
    }

    uint32_t sum = 0;
    for (uint32_t value : bitrate_history_) {
        sum += value;
    }
    uint32_t average_kbps = sum / bitrate_history_.size();

    const uint32_t common_bitrates[] = {32, 64, 96, 128, 160, 192, 256, 320};
    uint32_t best_match = common_bitrates[0];
    uint32_t min_diff = std::abs((int)average_kbps - (int)best_match);

    for (size_t i = 1; i < sizeof(common_bitrates) / sizeof(common_bitrates[0]); ++i) {
        uint32_t diff = std::abs((int)average_kbps - (int)common_bitrates[i]);
        if (diff < min_diff) {
            min_diff = diff;
            best_match = common_bitrates[i];
        }
    }

    uint32_t reference_bitrate = bitrate_adapted_once_ ? detected_bitrate_kbps_ : DEFAULT_BITRATE_KBPS;
    uint32_t gap = reference_bitrate > best_match ? reference_bitrate - best_match : best_match - reference_bitrate;
    uint32_t threshold = std::max<uint32_t>(8, reference_bitrate / 10);

    if (!bitrate_adapted_once_ || gap > threshold) {
        LOG_INFO("Bitrate auto-detected: %u kbps (avg %u kbps, sample %u kbps)",
                 best_match, average_kbps, measured_kbps);
        calculate_adaptive_sizes(best_match);
        bitrate_adapted_once_ = true;
    }
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
    current_playback_chunk_abs_id_ = INVALID_CHUNK_ABS_ID;
    playback_chunk_loaded_size_ = 0;
    pending_chunks_.clear();
    ready_chunks_.clear();
    pause_download_ = false;
    is_auto_paused_ = false;
    cumulative_time_ms_ = 0;  // Reset temporal tracking

    // Reset bitrate tracking so each stream starts from the default assumption
    bitrate_history_.clear();
    bytes_since_rate_sample_ = 0;
    bitrate_sample_start_ms_ = 0;
    bitrate_adapted_once_ = false;
    calculate_adaptive_sizes(DEFAULT_BITRATE_KBPS);

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

    recording_buffer_ = (uint8_t*)malloc(dynamic_buffer_size_);
    if (!recording_buffer_) {
        LOG_ERROR("Failed to allocate recording buffer (%u KB)", (unsigned)(dynamic_buffer_size_ / 1024));
        close();
        return false;
    }

    playback_buffer_ = (uint8_t*)malloc(dynamic_playback_buffer_size_);
    if (!playback_buffer_) {
        LOG_ERROR("Failed to allocate playback buffer (%u KB)", (unsigned)(dynamic_playback_buffer_size_ / 1024));
        free(recording_buffer_);
        recording_buffer_ = nullptr;
        close();
        return false;
    }

    LOG_INFO("Timeshift buffers allocated: rec=%uKB, play=%uKB (adaptive for %u kbps)",
             (unsigned)(dynamic_buffer_size_ / 1024),
             (unsigned)(dynamic_playback_buffer_size_ / 1024),
             detected_bitrate_kbps_);
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
    // CRITICAL FIX: Increased stack from 8KB to 24KB to prevent stack overflow
    // HTTPClient + WiFiClient + TLS + local buffers need substantial stack space
    BaseType_t result = xTaskCreate(download_task_trampoline, "ts_download", 24576, this, 5, &download_task_handle_);
    if (result != pdPASS) {
        LOG_ERROR("Failed to create download task");
        is_running_ = false;
        return false;
    }
    LOG_INFO("TimeshiftManager download task created successfully");

    // Crea il nuovo task di pre-caricamento (increased from 4KB to 8KB for safety)
    result = xTaskCreate(preloader_task_trampoline, "ts_preloader", 8192, this, 4, &preloader_task_handle_);
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

    // Reset preload state after seek
    last_preload_check_chunk_abs_id_ = INVALID_CHUNK_ABS_ID;

    // Verify that the offset is in a READY chunk
    uint32_t abs_chunk_id = find_chunk_for_offset(position);
    if (abs_chunk_id == INVALID_CHUNK_ABS_ID) {
        xSemaphoreGive(mutex_);
        LOG_WARN("Seek to %u failed: offset not in ready chunks", (unsigned)position);
        return false;
    }

    // Update read offset (next read() will load the correct chunk)
    current_read_offset_ = position;

    xSemaphoreGive(mutex_);
    LOG_INFO("Seek to offset %u (chunk abs ID %u)", (unsigned)position, abs_chunk_id);
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

    // Conservative estimate: num_chunks * dynamic_chunk_size_
    // (actual calculation would need mutex, but this is good enough for "ready?" check)
    return num_ready * dynamic_chunk_size_;
}

size_t TimeshiftManager::total_downloaded_bytes() const {
    return current_recording_offset_;
}

float TimeshiftManager::buffer_duration_seconds() const {
    float bitrate_kbps = detected_bitrate_kbps_ != 0 ? (float)detected_bitrate_kbps_ : (float)DEFAULT_BITRATE_KBPS;
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
    uint32_t last_playback_chunk_abs_id_seen = INVALID_CHUNK_ABS_ID;
    bool next_chunk_preloaded = false;

    while (is_running_) {
        vTaskDelay(pdMS_TO_TICKS(100)); // Controlla ogni 100ms

        xSemaphoreTake(mutex_, portMAX_DELAY);

        if (current_playback_chunk_abs_id_ == INVALID_CHUNK_ABS_ID || ready_chunks_.empty()) {
            xSemaphoreGive(mutex_);
            continue;
        }

        // Rileva se siamo passati a un nuovo chunk
        if (current_playback_chunk_abs_id_ != last_playback_chunk_abs_id_seen) {
            last_playback_chunk_abs_id_seen = current_playback_chunk_abs_id_;
            next_chunk_preloaded = false; // Reset: il nuovo chunk successivo non è ancora stato precaricato
            LOG_DEBUG("Preloader: switched to chunk abs ID %u, will preload %u when ready",
                      current_playback_chunk_abs_id_,
                      current_playback_chunk_abs_id_ + 1);
        }

        // Se il chunk successivo non è ancora stato pre-caricato, controlla se è il momento giusto
        if (!next_chunk_preloaded) {
            // Trova il chunk corrente nell'array
            size_t current_idx = find_chunk_index_by_id(current_playback_chunk_abs_id_);
            if (current_idx == INVALID_CHUNK_ID) {
                xSemaphoreGive(mutex_);
                continue; // Chunk corrente non trovato (rimosso?)
            }

            // Verifica che il chunk successivo esista
            if (current_idx + 1 >= ready_chunks_.size()) {
                xSemaphoreGive(mutex_);
                continue; // Chunk successivo non ancora disponibile
            }

            // Calcola il progresso nel chunk corrente
            const auto& current_chunk = ready_chunks_[current_idx];
            size_t offset_in_chunk = current_read_offset_ - current_chunk.start_offset;

            // Protezione contro offset negativi (può capitare subito dopo uno switch)
            if (current_read_offset_ < current_chunk.start_offset) {
                xSemaphoreGive(mutex_);
                continue;
            }

            float progress = (float)offset_in_chunk / (float)current_chunk.length;

            // Trigger di pre-caricamento (al 50% del chunk corrente)
            if (progress >= 0.50f) {
                uint32_t next_abs_id = current_playback_chunk_abs_id_ + 1;
                if (preload_next_chunk(current_playback_chunk_abs_id_)) {
                    LOG_DEBUG("Preloader task loaded chunk abs ID %u", next_abs_id);
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

    // CRITICAL: Verify stream pointer is valid
    if (!stream) {
        LOG_ERROR("CRITICAL: getStreamPtr() returned NULL!");
        is_running_ = false;
        http.end();
        vTaskDelete(nullptr);
        return;
    }

    uint8_t buf[dynamic_download_chunk_];
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

        // SAFETY: Check stream validity before using
        if (!stream || !stream->connected()) {
            LOG_WARN("Stream disconnected, attempting reconnect...");
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
            if (!stream) {
                LOG_ERROR("Reconnection failed: NULL stream");
                break;
            }
            last_data_time = millis();
            LOG_INFO("Reconnected successfully");
            continue;
        }

        int available = stream->available();
        if (available > 0) {
            // OVERFLOW PROTECTION: Check if we have enough space before reading
            size_t space_left = dynamic_buffer_size_ - bytes_in_current_chunk_;
            if (space_left < dynamic_download_chunk_) {
                // Buffer is critically full, force flush NOW
                LOG_WARN("OVERFLOW PROTECTION: Buffer critically full (%u/%u bytes), forcing immediate flush",
                         (unsigned)bytes_in_current_chunk_, (unsigned)dynamic_buffer_size_);
                if (!flush_recording_chunk()) {
                    LOG_ERROR("Emergency flush failed!");
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue; // Skip this iteration, don't write more data
                }
                // After flush, space_left is reset
                space_left = dynamic_buffer_size_;
            }

            // Limit read to available space (defensive)
            size_t to_read = std::min({sizeof(buf), (size_t)available, space_left});
            int len = stream->readBytes(buf, to_read);

            if (len > 0) {
                uint32_t now = millis();
                last_data_time = now; // Reset timeout on successful read

                if (bytes_since_rate_sample_ == 0) {
                    bitrate_sample_start_ms_ = now;
                }
                bytes_since_rate_sample_ += len;

                uint32_t rate_elapsed_ms = now - bitrate_sample_start_ms_;
                if (rate_elapsed_ms >= BITRATE_SAMPLE_WINDOW_MS) {
                    uint32_t measured_kbps = (bytes_since_rate_sample_ * 8) / rate_elapsed_ms;
                    apply_bitrate_measurement(measured_kbps);
                    bytes_since_rate_sample_ = 0;
                    bitrate_sample_start_ms_ = 0;
                }

                // Write to recording_buffer_ (circular) with bounds check
                for (int i = 0; i < len; i++) {
                    if (bytes_in_current_chunk_ >= dynamic_buffer_size_) {
                        // This should NEVER happen due to checks above, but defensive programming
                        LOG_ERROR("CRITICAL: Buffer overflow prevented! bytes=%u, dynamic_buffer_size_=%u",
                                 (unsigned)bytes_in_current_chunk_, (unsigned)dynamic_buffer_size_);
                        break; // Stop writing immediately
                    }
                    recording_buffer_[rec_write_head_] = buf[i];
                    rec_write_head_ = (rec_write_head_ + 1) % dynamic_buffer_size_;
                    bytes_in_current_chunk_++;
                }
                total_downloaded += len;

                // --- LOGICA DI FLUSH DECOUPLED ---
                // Controlliamo se è necessario un flush, ma lo eseguiamo *fuori* dal mutex
                // per non bloccare il thread di playback durante la scrittura su SD.
                bool needs_flush = (bytes_in_current_chunk_ >= dynamic_min_flush_size_);

                if (needs_flush) {
                    LOG_INFO("Buffer reached target (%u bytes), flushing chunk", (unsigned)bytes_in_current_chunk_);
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
                LOG_WARN("Stream timeout (no data for %u sec), will reconnect on next iteration", STREAM_TIMEOUT / 1000);
                // Don't reconnect here - the stream validity check at loop start will handle it
                last_data_time = millis(); // Reset to avoid spam
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
        // Data wraps around: [dynamic_buffer_size_ - remainder ... dynamic_buffer_size_) + [0 ... rec_write_head_)
        size_t remainder = chunk.length - rec_write_head_;
        start_pos = dynamic_buffer_size_ - remainder;

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
        // Data wraps around: [dynamic_buffer_size_ - remainder ... dynamic_buffer_size_) + [0 ... rec_write_head_)
        size_t remainder = chunk.length - rec_write_head_;
        start_pos = dynamic_buffer_size_ - remainder;

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
                                                 uint32_t& out_duration_ms,
                                                 uint32_t& out_bitrate_kbps)
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
    
    // Extract bitrate from first valid MP3 frame (already parsed above)
    // The bitrate_kbps was calculated in the frame parsing loop
    // For now, estimate from chunk size and duration
    if (out_duration_ms > 0) {
        out_bitrate_kbps = (chunk.length * 8) / (out_duration_ms / 1000);
    } else {
        out_bitrate_kbps = 128;  // Default fallback
    }

    LOG_DEBUG("Chunk %u: %u samples, %u ms @ %u Hz, bitrate ~%u kbps",
              chunk.id, out_frames, out_duration_ms, sample_rate, out_bitrate_kbps);

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
    uint32_t extracted_bitrate_kbps = 0;

    if (calculate_chunk_duration(chunk, total_frames, duration_ms, extracted_bitrate_kbps)) {
        chunk.total_frames = total_frames;
        chunk.duration_ms = duration_ms;
        chunk.start_time_ms = cumulative_time_ms_;

        cumulative_time_ms_ += duration_ms;  // Update cumulative time

        // Use extracted bitrate for initial sizing if not yet adapted
        if (!bitrate_adapted_once_ && extracted_bitrate_kbps > 0) {
            LOG_INFO("Bitrate extracted from first chunk header: %u kbps", extracted_bitrate_kbps);
            calculate_adaptive_sizes(extracted_bitrate_kbps);
            bitrate_adapted_once_ = true;
        }

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
    bool playback_chunk_removed = false;

    while (!ready_chunks_.empty()) {
        const ChunkInfo& oldest = ready_chunks_.front();
        size_t age_bytes = current_recording_offset_ - oldest.end_offset;
        size_t age_mb = age_bytes / (1024 * 1024);

        LOG_DEBUG("Checking oldest chunk abs ID %u: end_offset=%u MB, age=%u MB (%u bytes)",
                  oldest.id,
                  (unsigned)(oldest.end_offset / (1024 * 1024)),
                  (unsigned)age_mb,
                  (unsigned)age_bytes);

        // --- SAFETY CHECK ---
        // Do not remove the currently playing chunk or the next 2 chunks.
        // This creates a "safe zone" to prevent deleting data that is about to be played.
        if (oldest.id >= current_playback_chunk_abs_id_ && oldest.id <= current_playback_chunk_abs_id_ + 2) {
            LOG_DEBUG("Oldest chunk abs ID %u is in the playback safe zone, stopping cleanup.", oldest.id);
            break;
        }

        if (age_bytes > MAX_TS_WINDOW) {
            LOG_INFO("CLEANUP: Removing old chunk abs ID %u (age: %u MB > limit: %u MB)",
                     oldest.id,
                     (unsigned)age_mb,
                     (unsigned)(MAX_TS_WINDOW / (1024 * 1024)));

            // Check if we're removing the currently playing chunk
            if (current_playback_chunk_abs_id_ == oldest.id) {
                playback_chunk_removed = true;
            }

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
            LOG_DEBUG("Oldest chunk abs ID %u is still within window (age: %u MB <= limit: %u MB), stopping cleanup",
                      oldest.id,
                      (unsigned)age_mb,
                      (unsigned)(MAX_TS_WINDOW / (1024 * 1024)));
            break; // Chunks are ordered, so we can stop
        }
    }

    // NEW APPROACH: No more index adjustments! Just validate absolute IDs
    if (removed_count > 0) {
        if (playback_chunk_removed) {
            LOG_WARN("CLEANUP: Current playback chunk was deleted");
            current_playback_chunk_abs_id_ = INVALID_CHUNK_ABS_ID;
            last_preload_check_chunk_abs_id_ = INVALID_CHUNK_ABS_ID;
        }

        // Validate that current read offset is still in valid range
        if (!ready_chunks_.empty()) {
            const ChunkInfo& first_chunk = ready_chunks_.front();
            const ChunkInfo& last_chunk = ready_chunks_.back();

            // If read offset is before first chunk or after last chunk, jump to first
            if (current_read_offset_ < first_chunk.start_offset ||
                current_read_offset_ >= last_chunk.end_offset) {

                size_t old_offset = current_read_offset_;
                current_read_offset_ = first_chunk.start_offset;
                current_playback_chunk_abs_id_ = INVALID_CHUNK_ABS_ID;  // Force reload

                LOG_WARN("CLEANUP: Playback jumped from offset %u to %u (live stream caught up)",
                         (unsigned)old_offset, (unsigned)current_read_offset_);
            }
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

// ========== HELPER: Convert absolute chunk ID to array index ==========
size_t TimeshiftManager::find_chunk_index_by_id(uint32_t abs_chunk_id) {
    for (size_t i = 0; i < ready_chunks_.size(); i++) {
        if (ready_chunks_[i].id == abs_chunk_id) {
            return i;
        }
    }
    return INVALID_CHUNK_ID;  // Not found
}

bool TimeshiftManager::preload_next_chunk(uint32_t current_abs_chunk_id) {
    // Find next chunk by absolute ID (current + 1)
    uint32_t next_abs_chunk_id = current_abs_chunk_id + 1;

    // Find the next chunk in the array
    size_t next_idx = INVALID_CHUNK_ID;
    for (size_t i = 0; i < ready_chunks_.size(); i++) {
        if (ready_chunks_[i].id == next_abs_chunk_id) {
            next_idx = i;
            break;
        }
    }

    // Il preloader task non attende, controlla solo se il chunk è disponibile
    if (next_idx == INVALID_CHUNK_ID) {
        return false; // Non è ancora pronto, riproverà al prossimo ciclo
    }

    const ChunkInfo& next_chunk = ready_chunks_[next_idx];
    if (next_chunk.state != ChunkState::READY) { // Controllo di sicurezza
        LOG_WARN("Preload failed: chunk abs ID %u is not in READY state", next_abs_chunk_id);
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
        size_t read = file.read(playback_buffer_ + dynamic_chunk_size_, next_chunk.length);
        file.close();

        if (read != next_chunk.length) {
            LOG_ERROR("Preload read mismatch: expected %u, got %u", next_chunk.length, read);
            return false;
        }
    } else {
        // PSRAM mode: direct copy from PSRAM pool
        if (!next_chunk.psram_ptr) {
            LOG_ERROR("Preload failed: null PSRAM pointer for chunk abs ID %u", next_abs_chunk_id);
            return false;
        }
        memcpy(playback_buffer_ + dynamic_chunk_size_, next_chunk.psram_ptr, next_chunk.length);
    }

    LOG_DEBUG("Preloaded chunk abs ID %u (%u KB) at buffer offset %u",
              next_abs_chunk_id, next_chunk.length / 1024, (unsigned)dynamic_chunk_size_);

    return true;
}

// ========== PLAYBACK SIDE METHODS ==========

uint32_t TimeshiftManager::find_chunk_for_offset(size_t offset) {
    // Returns absolute chunk ID (NOT index) for the chunk containing the offset
    if (ready_chunks_.empty()) {
        return INVALID_CHUNK_ABS_ID;
    }

    // Binary search per trovare il primo chunk il cui end_offset è > offset
    size_t low = 0, high = ready_chunks_.size();
    size_t best_match_idx = INVALID_CHUNK_ID;

    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (ready_chunks_[mid].start_offset > offset) {
            high = mid;
        } else {
            best_match_idx = mid;
            low = mid + 1;
        }
    }

    if (best_match_idx != INVALID_CHUNK_ID) {
        const auto& chunk = ready_chunks_[best_match_idx];
        // Se l'offset è nel chunk o in un piccolo gap prima del successivo, è valido.
        // Il "gap" massimo tollerato è di 4KB, più che sufficiente.
        if (offset <= chunk.end_offset || (best_match_idx + 1 < ready_chunks_.size() && offset < ready_chunks_[best_match_idx + 1].start_offset + 4096)) {
            return chunk.id;  // Return absolute ID, not index!
        }
    }

    // Se arriviamo qui, l'offset è veramente fuori range. Logghiamo per debug.
    LOG_ERROR("STUTTER DETECTED: No chunk found for offset %u. Last chunk ends at %u.",
              (unsigned)offset, (unsigned)ready_chunks_.back().end_offset);
    return INVALID_CHUNK_ABS_ID;
}

bool TimeshiftManager::load_chunk_to_playback(uint32_t abs_chunk_id) {
    // Find chunk by absolute ID
    size_t chunk_idx = find_chunk_index_by_id(abs_chunk_id);
    if (chunk_idx == INVALID_CHUNK_ID) {
        LOG_ERROR("Invalid chunk absolute ID: %u (not found in ready_chunks_)", abs_chunk_id);
        return false;
    }

    const ChunkInfo& chunk = ready_chunks_[chunk_idx];
    if (chunk.state != ChunkState::READY) {
        LOG_ERROR("Chunk abs ID %u is not READY (state: %d)", abs_chunk_id, (int)chunk.state);
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
            LOG_ERROR("Failed to load chunk abs ID %u: null PSRAM pointer", abs_chunk_id);
            return false;
        }
        memcpy(playback_buffer_, chunk.psram_ptr, chunk.length);
    }

    // Update playback state with ABSOLUTE ID
    current_playback_chunk_abs_id_ = abs_chunk_id;
    playback_chunk_loaded_size_ = chunk.length;

    // Log with temporal info for better user understanding
    uint32_t start_min = chunk.start_time_ms / 60000;
    uint32_t start_sec = (chunk.start_time_ms / 1000) % 60;
    uint32_t end_ms = chunk.start_time_ms + chunk.duration_ms;
    uint32_t end_min = end_ms / 60000;
    uint32_t end_sec = (end_ms / 1000) % 60;

    LOG_INFO("→ Loaded chunk abs ID %u (%u KB) [%02u:%02u - %02u:%02u]",
             abs_chunk_id, chunk.length / 1024,
             start_min, start_sec, end_min, end_sec);

    return true;
}

size_t TimeshiftManager::read_from_playback_buffer(size_t offset, void* buffer, size_t size) {
    // Find chunk containing this offset (returns ABSOLUTE chunk ID)
    uint32_t abs_chunk_id = find_chunk_for_offset(offset);
    if (abs_chunk_id == INVALID_CHUNK_ABS_ID) {
        // For LIVE timeshift: if we're beyond available chunks but download is still running,
        // WAIT for new chunks instead of returning 0 (which signals EOF to audio player)
        if (is_running_ && !ready_chunks_.empty()) {
            size_t last_chunk_end = ready_chunks_.back().end_offset;
            if (offset >= last_chunk_end - 4096) {  // Within 4KB of end
                LOG_INFO("Playback catching up to live stream, waiting for next chunk...");

                // Wait up to 3 seconds for new chunk
                uint32_t wait_start = millis();
                const uint32_t MAX_WAIT_MS = 3000;
                size_t initial_chunk_count = ready_chunks_.size();

                while (is_running_ && (millis() - wait_start) < MAX_WAIT_MS) {
                    xSemaphoreGive(mutex_);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    xSemaphoreTake(mutex_, portMAX_DELAY);

                    // Check if new chunk arrived
                    if (ready_chunks_.size() > initial_chunk_count) {
                        abs_chunk_id = find_chunk_for_offset(offset);
                        if (abs_chunk_id != INVALID_CHUNK_ABS_ID) {
                            LOG_INFO("New chunk arrived, resuming playback");
                            break;
                        }
                    }
                }

                // If still not found after waiting, it's a real problem
                if (abs_chunk_id == INVALID_CHUNK_ABS_ID) {
                    LOG_WARN("No chunk found for offset %u after waiting", (unsigned)offset);
                    return 0;
                }
            } else {
                LOG_WARN("No chunk found for offset %u", (unsigned)offset);
                return 0;
            }
        } else {
            LOG_WARN("No chunk found for offset %u", (unsigned)offset);
            return 0;
        }
    }

    // Get chunk index for accessing chunk info
    size_t chunk_idx = find_chunk_index_by_id(abs_chunk_id);
    if (chunk_idx == INVALID_CHUNK_ID) {
        LOG_ERROR("CRITICAL: Found abs chunk ID %u but can't find it in array!", abs_chunk_id);
        return 0;
    }

    // Se il chunk richiesto è quello successivo (abs ID = current + 1), esegui lo switch "seamless"
    if (current_playback_chunk_abs_id_ != INVALID_CHUNK_ABS_ID &&
        abs_chunk_id == current_playback_chunk_abs_id_ + 1) {

        // Seamless switch: il chunk è già stato pre-caricato
        const ChunkInfo& preloaded_chunk_info = ready_chunks_[chunk_idx];
        size_t preloaded_size = preloaded_chunk_info.length;
        // Sposta il chunk pre-caricato (che si trova nella seconda metà del buffer) all'inizio.
        memmove(playback_buffer_, playback_buffer_ + dynamic_chunk_size_, preloaded_size);

        current_playback_chunk_abs_id_ = abs_chunk_id;
        playback_chunk_loaded_size_ = preloaded_size;
        last_preload_check_chunk_abs_id_ = INVALID_CHUNK_ABS_ID; // Resetta il tracking per il nuovo chunk
        LOG_DEBUG("Switching to preloaded chunk abs ID %u (seamless)", abs_chunk_id);

    } else if (current_playback_chunk_abs_id_ != abs_chunk_id) {
        // Seek o situazione anomala: carica il chunk richiesto da SD (potrebbe causare stutter)
        LOG_WARN("Chunk abs ID %u not preloaded, loading now (may cause stutter)", abs_chunk_id);

        // Attiva la pausa automatica per buffering se il callback è registrato
        if (auto_pause_callback_ && !is_auto_paused_) {
            LOG_INFO("Auto-pausing playback for buffering...");
            is_auto_paused_ = true;
            auto_pause_callback_(true);  // true = pause
        }

        if (!load_chunk_to_playback(abs_chunk_id)) {
            LOG_ERROR("Failed to load chunk abs ID %u for playback", abs_chunk_id);
            return 0;
        }

        // Attendi il margine configurato per permettere al preloader di caricare i chunk successivi
        if (auto_pause_callback_ && is_auto_paused_) {
            // Se ENTRAMBI i margini sono 0, riprendi immediatamente senza attese
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
                    size_t target_chunks = ready_chunks_.size() + auto_pause_min_chunks_;

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
    const ChunkInfo& chunk = ready_chunks_[chunk_idx];
    size_t chunk_offset = offset - chunk.start_offset;
    size_t available = playback_chunk_loaded_size_ - chunk_offset;
    size_t to_read = std::min(size, available);

    // Copia i dati dal buffer di playback
    memcpy(buffer, playback_buffer_ + chunk_offset, to_read);

    return to_read;
}

// ========== TEMPORAL SEEK METHODS ==========

size_t TimeshiftManager::seek_to_time(uint32_t target_ms) {
    // --- NUOVA LOGICA DI SEEK RELATIVO ---
    // Tratta sempre il target_ms come un offset relativo alla durata totale del buffer disponibile.
    // Esempio: se il buffer contiene 60 secondi di audio e si chiede un seek a 10000ms (10s),
    // il seek avverrà a 10 secondi dall'inizio del primo chunk disponibile.

    xSemaphoreTake(mutex_, portMAX_DELAY);

    if (ready_chunks_.empty()) {
        xSemaphoreGive(mutex_);
        LOG_WARN("Seek to time failed: no ready chunks available");
        return SIZE_MAX; // Invalid offset
    }

    // 1. Calcola la durata totale del buffer disponibile
    uint32_t total_duration_ms = 0;
    for (const auto& c : ready_chunks_) {
        total_duration_ms += c.duration_ms;
    }

    // 2. Limita (clampa) il target alla durata disponibile
    if (target_ms >= total_duration_ms) {
        LOG_WARN("Seek target %u ms is beyond available duration %u ms. Seeking to the end.",
                 target_ms, total_duration_ms);
        target_ms = total_duration_ms > 0 ? total_duration_ms - 1 : 0; // Vai alla fine
    }

    // 3. Itera sui chunk per trovare la posizione corretta
    uint32_t accumulated_duration_ms = 0;
    for (const auto& chunk : ready_chunks_) {
        if (target_ms < accumulated_duration_ms + chunk.duration_ms) {
            // Trovato il chunk corretto!
            uint32_t time_into_chunk = target_ms - accumulated_duration_ms;
            float progress_in_chunk = (float)time_into_chunk / (float)chunk.duration_ms;

            // Stima l'offset in byte basato sulla progressione temporale (interpolazione lineare)
            size_t byte_offset_in_chunk = (size_t)(chunk.length * progress_in_chunk);
            size_t final_offset = chunk.start_offset + byte_offset_in_chunk;

            LOG_INFO("Seek to %u ms (relative) -> chunk %u, byte offset %u (progress %.1f%%)",
                     target_ms, chunk.id, (unsigned)final_offset, progress_in_chunk * 100.0f);

            xSemaphoreGive(mutex_);
            return final_offset;
        }
        accumulated_duration_ms += chunk.duration_ms;
    }

    // Fallback: se qualcosa va storto, vai all'inizio dell'ultimo chunk
    LOG_WARN("Seek failed to find position, falling back to last chunk start.");
    size_t fallback_offset = ready_chunks_.back().start_offset;
    xSemaphoreGive(mutex_);
    return fallback_offset;
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
    psram_pool_size_ = MAX_PSRAM_CHUNKS * dynamic_chunk_size_;

    // Try to allocate in PSRAM first (heap_caps_malloc with MALLOC_CAP_SPIRAM)
    psram_chunk_pool_ = (uint8_t*)heap_caps_malloc(psram_pool_size_, MALLOC_CAP_SPIRAM);

    if (!psram_chunk_pool_) {
        LOG_ERROR("Failed to allocate %u KB in PSRAM", psram_pool_size_ / 1024);
        psram_pool_size_ = 0;
        return false;
    }

    LOG_INFO("PSRAM pool allocated: %u KB (%u chunks x %u KB)",
             psram_pool_size_ / 1024, MAX_PSRAM_CHUNKS, dynamic_chunk_size_ / 1024);

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
    uint8_t* chunk_ptr = psram_chunk_pool_ + (pool_index * dynamic_chunk_size_);

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
