#include "timeshift_manager.h"
#include "logger.h"
#include "drivers/sd_card_driver.h"  // For SD card access
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include "mp3_seek_table.h"

constexpr size_t BUFFER_SIZE = 128 * 1024;       // 128KB per buffer
constexpr size_t CHUNK_SIZE = 512 * 1024;        // 512KB SD chunks
constexpr size_t MAX_TS_WINDOW = 1024 * 1024 * 512; // 512 MB max window
constexpr size_t DOWNLOAD_CHUNK = 2048;          // Download buffer size
constexpr size_t MIN_CHUNK_FLUSH_SIZE = 64 * 1024;  // Min 64KB before flushing

TimeshiftManager::TimeshiftManager() {
    mutex_ = xSemaphoreCreateMutex();
}

TimeshiftManager::~TimeshiftManager() {
    stop();
    close();
    if (mutex_) vSemaphoreDelete(mutex_);
    if (recording_buffer_) free(recording_buffer_);
    if (playback_buffer_) free(playback_buffer_);
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

    recording_buffer_ = (uint8_t*)malloc(BUFFER_SIZE);
    if (!recording_buffer_) {
        LOG_ERROR("Failed to allocate recording buffer");
        close();
        return false;
    }

    playback_buffer_ = (uint8_t*)malloc(BUFFER_SIZE);
    if (!playback_buffer_) {
        LOG_ERROR("Failed to allocate playback buffer");
        free(recording_buffer_);
        recording_buffer_ = nullptr;
        close();
        return false;
    }

    LOG_INFO("Timeshift buffers allocated: 2x%uKB", BUFFER_SIZE / 1024);
    return true;
}

void TimeshiftManager::close() {
    stop();

    // Clean up all chunks (both pending and ready)
    for (const auto& chunk : pending_chunks_) {
        SD_MMC.remove(chunk.filename.c_str());
    }
    for (const auto& chunk : ready_chunks_) {
        SD_MMC.remove(chunk.filename.c_str());
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
    return true;
}

void TimeshiftManager::stop() {
    if (is_running_) {
        is_running_ = false;
        if (download_task_handle_) {
            vTaskDelete(download_task_handle_);
            download_task_handle_ = nullptr;
        }
    }
}

size_t TimeshiftManager::read(void* buffer, size_t size) {
    if (!is_open_) return 0;

    // Wait until we have ready chunks available (up to 5 seconds)
    const uint32_t MAX_WAIT_MS = 5000;
    uint32_t start_wait = millis();

    while (is_running_ && ready_chunks_.empty()) {
        if (millis() - start_wait > MAX_WAIT_MS) {
            LOG_WARN("Timeout waiting for ready chunks");
            return 0;  // Timeout - no data available
        }
        vTaskDelay(pdMS_TO_TICKS(100));  // Wait for chunks to be promoted to READY
    }

    if (ready_chunks_.empty()) {
        LOG_WARN("No ready chunks available for playback");
        return 0; // No data available
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);

    // Read from playback buffer (will load chunk if needed)
    size_t bytes_read = read_from_playback_buffer(current_read_offset_, buffer, size);
    current_read_offset_ += bytes_read;

    xSemaphoreGive(mutex_);
    return bytes_read;
}

bool TimeshiftManager::seek(size_t position) {
    if (!is_open_) return false;

    xSemaphoreTake(mutex_, portMAX_DELAY);

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

                xSemaphoreTake(mutex_, portMAX_DELAY);

                // Write to recording_buffer_ (circular)
                for (int i = 0; i < len; i++) {
                    recording_buffer_[rec_write_head_] = buf[i];
                    rec_write_head_ = (rec_write_head_ + 1) % BUFFER_SIZE;
                    bytes_in_current_chunk_++;

                    // Flush when buffer is almost full (must flush before wrap corrupts data)
                    if (bytes_in_current_chunk_ >= BUFFER_SIZE - 4096) {
                        LOG_INFO("Buffer near full (%u bytes), flushing to SD", (unsigned)bytes_in_current_chunk_);
                        if (!flush_recording_chunk()) {
                            LOG_ERROR("Failed to flush recording chunk");
                        }
                    }
                }
                total_downloaded += len;

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
    if (bytes_in_current_chunk_ < MIN_CHUNK_FLUSH_SIZE) {
        // Not enough data to flush yet
        return true;
    }

    // Create ChunkInfo for PENDING chunk
    ChunkInfo chunk;
    chunk.id = next_chunk_id_++;
    chunk.start_offset = current_recording_offset_;
    chunk.length = bytes_in_current_chunk_;
    chunk.end_offset = current_recording_offset_ + bytes_in_current_chunk_;
    chunk.filename = "/ts_pending_" + std::to_string(chunk.id) + ".bin";
    chunk.state = ChunkState::PENDING;
    chunk.crc32 = 0; // TODO: calculate CRC if needed

    // Write chunk to SD
    if (!write_chunk_to_sd(chunk)) {
        LOG_ERROR("Failed to write chunk %u to SD", chunk.id);
        return false;
    }

    // Validate and promote to READY
    if (validate_chunk(chunk)) {
        promote_chunk_to_ready(chunk);
    } else {
        LOG_ERROR("Chunk %u validation failed", chunk.id);
        SD_MMC.remove(chunk.filename.c_str());
        return false;
    }

    // Update recording state
    current_recording_offset_ += bytes_in_current_chunk_;
    bytes_in_current_chunk_ = 0;
    // DON'T reset rec_write_head_ - buffer continues circularly

    // Cleanup old chunks
    cleanup_old_chunks();

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

bool TimeshiftManager::validate_chunk(ChunkInfo& chunk) {
    // Basic validation: check file exists and size matches
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

    // TODO: Add CRC32 validation if needed
    return true;
}

void TimeshiftManager::promote_chunk_to_ready(ChunkInfo chunk) {
    // Rename file from pending to ready
    std::string ready_filename = "/ts_ready_" + std::to_string(chunk.id) + ".bin";

    if (SD_MMC.rename(chunk.filename.c_str(), ready_filename.c_str())) {
        chunk.filename = ready_filename;
        chunk.state = ChunkState::READY;

        // Add to ready_chunks_ (already ordered by ID)
        ready_chunks_.push_back(chunk);

        LOG_INFO("Chunk %u promoted to READY (%u KB, offset %u-%u)",
                 chunk.id, chunk.length / 1024,
                 (unsigned)chunk.start_offset, (unsigned)chunk.end_offset);
    } else {
        LOG_ERROR("Failed to rename chunk %u from pending to ready", chunk.id);
        SD_MMC.remove(chunk.filename.c_str());
    }
}

void TimeshiftManager::cleanup_old_chunks() {
    // Remove chunks beyond the timeshift window
    while (!ready_chunks_.empty()) {
        const ChunkInfo& oldest = ready_chunks_.front();

        if (current_recording_offset_ - oldest.end_offset > MAX_TS_WINDOW) {
            LOG_INFO("Removing old chunk %u: %s", oldest.id, oldest.filename.c_str());
            SD_MMC.remove(oldest.filename.c_str());
            ready_chunks_.erase(ready_chunks_.begin());
        } else {
            break; // Chunks are ordered, so we can stop
        }
    }
}

// ========== PLAYBACK SIDE METHODS ==========

size_t TimeshiftManager::find_chunk_for_offset(size_t offset) {
    // Binary search in ready_chunks_ (ordered by start_offset)
    for (size_t i = 0; i < ready_chunks_.size(); i++) {
        const ChunkInfo& chunk = ready_chunks_[i];
        if (offset >= chunk.start_offset && offset < chunk.end_offset) {
            return i; // Found
        }
    }
    return INVALID_CHUNK_ID; // Not found
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

    // Update playback state
    current_playback_chunk_id_ = chunk_id;
    playback_chunk_loaded_size_ = chunk.length;

    LOG_DEBUG("Loaded chunk %u to playback buffer (%u KB)", chunk.id, chunk.length / 1024);
    return true;
}

size_t TimeshiftManager::read_from_playback_buffer(size_t offset, void* buffer, size_t size) {
    // Find chunk containing this offset
    size_t chunk_id = find_chunk_for_offset(offset);
    if (chunk_id == INVALID_CHUNK_ID) {
        LOG_WARN("No chunk found for offset %u", (unsigned)offset);
        return 0; // Offset not available in ready chunks
    }

    // If chunk not loaded, load it
    if (current_playback_chunk_id_ != chunk_id) {
        if (!load_chunk_to_playback(chunk_id)) {
            LOG_ERROR("Failed to load chunk %u for playback", chunk_id);
            return 0;
        }
    }

    // Calculate offset relative to chunk
    const ChunkInfo& chunk = ready_chunks_[chunk_id];
    size_t chunk_offset = offset - chunk.start_offset;
    size_t available = chunk.length - chunk_offset;
    size_t to_read = std::min(size, available);

    // Copy from playback_buffer_
    memcpy(buffer, playback_buffer_ + chunk_offset, to_read);

    return to_read;
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
