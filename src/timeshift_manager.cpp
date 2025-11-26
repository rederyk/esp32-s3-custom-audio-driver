#include "timeshift_manager.h"
#include "logger.h"
#include "drivers/sd_card_driver.h"  // For SD card access
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include "mp3_seek_table.h"

constexpr size_t HOT_BUFFER_SIZE = 128 * 1024;   // 128KB RAM buffer (increased)
constexpr size_t CHUNK_SIZE = 512 * 1024;        // 512KB SD chunks
constexpr size_t MAX_TS_WINDOW = 1024 * 1024 * 512; // 512 MB max window
constexpr size_t DOWNLOAD_CHUNK = 2048;          // Download buffer size

TimeshiftManager::TimeshiftManager() {
    mutex_ = xSemaphoreCreateMutex();
}

TimeshiftManager::~TimeshiftManager() {
    stop();
    close();
    if (mutex_) vSemaphoreDelete(mutex_);
    if (hot_buffer_) free(hot_buffer_);
}

bool TimeshiftManager::open(const char* uri) {
    if (is_open_) close();
    
    uri_ = uri;
    is_open_ = true;
    current_download_offset_ = 0;
    current_read_offset_ = 0;
    hot_write_head_ = 0;
    hot_read_head_ = 0;
    chunks_.clear();
    
    hot_buffer_ = (uint8_t*)malloc(HOT_BUFFER_SIZE);
    if (!hot_buffer_) {
        LOG_ERROR("Failed to allocate hot buffer");
        close();
        return false;
    }
    
    return true;
}

void TimeshiftManager::close() {
    stop();
    
    // Clean up chunks
    for (const auto& chunk : chunks_) {
        SD_MMC.remove(chunk.filename.c_str());
    }
    chunks_.clear();
    
    if (hot_buffer_) {
        free(hot_buffer_);
        hot_buffer_ = nullptr;
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

    // For live streams, wait until we have data available (up to 5 seconds)
    const uint32_t MAX_WAIT_MS = 5000;
    uint32_t start_wait = millis();

    while (is_running_ && current_read_offset_ >= current_download_offset_) {
        if (millis() - start_wait > MAX_WAIT_MS) {
            LOG_WARN("Timeout waiting for download data");
            return 0;  // Timeout - no data available
        }
        vTaskDelay(pdMS_TO_TICKS(50));  // Wait for more data to be downloaded
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    size_t bytes_read = read_from_cache(current_read_offset_, buffer, size);
    current_read_offset_ += bytes_read;
    xSemaphoreGive(mutex_);
    return bytes_read;
}

bool TimeshiftManager::seek(size_t position) {
    if (!is_open_) return false;
    
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (position > current_download_offset_) {
        xSemaphoreGive(mutex_);
        return false;
    }
    current_read_offset_ = position;
    xSemaphoreGive(mutex_);
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
    return current_download_offset_;
}

bool TimeshiftManager::is_open() const {
    return is_open_;
}

const char* TimeshiftManager::uri() const {
    return uri_.c_str();
}


size_t TimeshiftManager::buffered_bytes() const {
    return current_download_offset_ - current_read_offset_;
}

size_t TimeshiftManager::total_downloaded_bytes() const {
    return current_download_offset_;
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
        int available = stream->available();
        if (available > 0) {
            size_t to_read = std::min(sizeof(buf), (size_t)available);
            int len = stream->readBytes(buf, to_read);

            if (len > 0) {
                last_data_time = millis(); // Reset timeout on successful read

                xSemaphoreTake(mutex_, portMAX_DELAY);

                // Write to hot buffer (circular)
                for (int i = 0; i < len; i++) {
                    hot_buffer_[hot_write_head_] = buf[i];
                    hot_write_head_ = (hot_write_head_ + 1) % HOT_BUFFER_SIZE;
                }
                current_download_offset_ += len;
                total_downloaded += len;

                // Log progress every 5 seconds
                if (millis() - last_log_time > 5000) {
                    LOG_INFO("Downloaded %u KB (total: %u KB, buffered: %u bytes)",
                             len / 1024, (unsigned)(total_downloaded / 1024),
                             (unsigned)buffered_bytes());
                    last_log_time = millis();
                }

                // Flush to SD if buffer is getting full (leave 32KB free for safety)
                size_t buffer_used = (hot_write_head_ >= hot_read_head_)
                    ? (hot_write_head_ - hot_read_head_)
                    : (HOT_BUFFER_SIZE - hot_read_head_ + hot_write_head_);

                if (buffer_used >= HOT_BUFFER_SIZE - 32 * 1024) {
                    LOG_INFO("Hot buffer near full (%u bytes), flushing to SD", (unsigned)buffer_used);
                    if (!flush_to_sd()) {
                        LOG_ERROR("Failed to flush to SD");
                    }
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

bool TimeshiftManager::flush_to_sd() {
    // Calculate how much data to flush (from hot_read_head_ to hot_write_head_)
    size_t bytes_to_flush;
    if (hot_write_head_ >= hot_read_head_) {
        bytes_to_flush = hot_write_head_ - hot_read_head_;
    } else {
        bytes_to_flush = (HOT_BUFFER_SIZE - hot_read_head_) + hot_write_head_;
    }

    if (bytes_to_flush < 64 * 1024) {
        // Not enough data to flush yet
        return true;
    }

    ChunkInfo new_chunk;
    new_chunk.id = chunks_.size();
    new_chunk.start_offset = current_download_offset_ - bytes_to_flush;
    new_chunk.length = bytes_to_flush;
    new_chunk.filename = "/ts_chunk_" + std::to_string(new_chunk.id) + ".bin";

    File file = SD_MMC.open(new_chunk.filename.c_str(), FILE_WRITE);
    if (!file) {
        LOG_ERROR("Failed to open chunk file for write: %s", new_chunk.filename.c_str());
        return false;
    }

    // Write data handling circular buffer
    size_t written = 0;
    if (hot_write_head_ >= hot_read_head_) {
        // Contiguous write
        written = file.write(hot_buffer_ + hot_read_head_, bytes_to_flush);
    } else {
        // Split write: from read_head to end, then from 0 to write_head
        size_t first_part = HOT_BUFFER_SIZE - hot_read_head_;
        written = file.write(hot_buffer_ + hot_read_head_, first_part);
        if (written == first_part && hot_write_head_ > 0) {
            written += file.write(hot_buffer_, hot_write_head_);
        }
    }

    file.close();

    if (written != bytes_to_flush) {
        LOG_ERROR("Failed to write chunk to SD (wrote %u of %u bytes)",
                  (unsigned)written, (unsigned)bytes_to_flush);
        SD_MMC.remove(new_chunk.filename.c_str());
        return false;
    }

    chunks_.push_back(new_chunk);
    hot_read_head_ = hot_write_head_; // Update read head

    LOG_INFO("Flushed chunk %u: %u bytes to %s",
             (unsigned)new_chunk.id, (unsigned)bytes_to_flush, new_chunk.filename.c_str());

    // Clean up old chunks if over max window
    while (!chunks_.empty() &&
           current_download_offset_ - chunks_.front().start_offset > MAX_TS_WINDOW) {
        LOG_INFO("Removing old chunk: %s", chunks_.front().filename.c_str());
        SD_MMC.remove(chunks_.front().filename.c_str());
        chunks_.erase(chunks_.begin());
    }

    return true;
}

size_t TimeshiftManager::read_from_cache(size_t offset, void* buffer, size_t size) {
    if (offset >= current_download_offset_) {
        // Trying to read beyond what's downloaded
        return 0;
    }

    size_t bytes_read = 0;
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    size_t remaining = size;

    // Step 1: Try to read from SD chunks first
    for (const auto& chunk : chunks_) {
        if (bytes_read >= size) break;

        if (offset >= chunk.start_offset && offset < chunk.start_offset + chunk.length) {
            size_t chunk_offset = offset - chunk.start_offset;
            size_t to_read = std::min(remaining, chunk.length - chunk_offset);

            File file = SD_MMC.open(chunk.filename.c_str(), FILE_READ);
            if (!file) {
                LOG_ERROR("Failed to open chunk file for read: %s", chunk.filename.c_str());
                return bytes_read;
            }

            if (!file.seek(chunk_offset)) {
                LOG_ERROR("Failed to seek in chunk file");
                file.close();
                return bytes_read;
            }

            size_t read = file.read(dst, to_read);
            file.close();

            bytes_read += read;
            if (read != to_read) {
                LOG_WARN("Partial read from chunk: %u of %u bytes", (unsigned)read, (unsigned)to_read);
                return bytes_read;
            }

            dst += read;
            offset += read;
            remaining -= read;
        }
    }

    // Step 2: Read remaining data from hot buffer (circular)
    if (remaining > 0 && offset < current_download_offset_) {
        // Calculate the oldest data in hot buffer
        size_t hot_buffer_size;
        if (hot_write_head_ >= hot_read_head_) {
            hot_buffer_size = hot_write_head_ - hot_read_head_;
        } else {
            hot_buffer_size = (HOT_BUFFER_SIZE - hot_read_head_) + hot_write_head_;
        }

        size_t hot_start = current_download_offset_ - hot_buffer_size;

        if (offset >= hot_start && offset < current_download_offset_) {
            // Data is in hot buffer
            size_t bytes_from_start = offset - hot_start;
            size_t physical_offset = (hot_read_head_ + bytes_from_start) % HOT_BUFFER_SIZE;
            size_t available = current_download_offset_ - offset;
            size_t to_read = std::min(remaining, available);

            // Handle circular buffer read
            for (size_t i = 0; i < to_read; i++) {
                dst[i] = hot_buffer_[(physical_offset + i) % HOT_BUFFER_SIZE];
            }

            bytes_read += to_read;
        }
    }

    return bytes_read;
}
