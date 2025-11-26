#include "timeshift_manager.h"
#include "logger.h"
#include "drivers/sd_card_driver.h"  // For SD card access
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include "mp3_seek_table.h"

constexpr size_t HOT_BUFFER_SIZE = 64 * 1024;    // 64KB RAM buffer
constexpr size_t CHUNK_SIZE = 512 * 1024;        // 512KB SD chunks
constexpr size_t MAX_TS_WINDOW = 1024 * 1024 * 512; // 512 MB max window

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
    if (!is_open_ || is_running_) return false;
    
    is_running_ = true;
    xTaskCreate(download_task_trampoline, "ts_download", 8192, this, 5, &download_task_handle_);
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
    HTTPClient http;
    http.begin(uri_.c_str());
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(5000);
    
    int httpCode = http.GET();
    if (httpCode <= 0) {
        LOG_ERROR("HTTP GET failed: %d", httpCode);
        is_running_ = false;
        return;
    }
    
    WiFiClient* stream = http.getStreamPtr();
    
    uint8_t buf[1024];
    while (is_running_) {
        int available = stream->available();
        if (available > 0) {
            int len = stream->read(buf, std::min(sizeof(buf), (size_t)available));
            if (len >  0) {
                xSemaphoreTake(mutex_, portMAX_DELAY);
                
                // Write to hot buffer
                size_t space = HOT_BUFFER_SIZE - hot_write_head_;
                size_t to_write = std::min((size_t)len, space);
                memcpy(hot_buffer_ + hot_write_head_, buf, to_write);
                hot_write_head_ += to_write;
                current_download_offset_ += to_write;
                
                if (to_write < len) {
                    memcpy(hot_buffer_, buf + to_write, len - to_write);
                    hot_write_head_ = len - to_write;
                    current_download_offset_ += len - to_write;
                }
                
                // Flush if full
                if (hot_write_head_ >= HOT_BUFFER_SIZE - 1024) {
                    flush_to_sd();
                }
                
                xSemaphoreGive(mutex_);
            }
        } else {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
    
    http.end();
}

bool TimeshiftManager::flush_to_sd() {
    ChunkInfo new_chunk;
    new_chunk.id = chunks_.size();
    new_chunk.start_offset = current_download_offset_ - hot_write_head_;
    new_chunk.length = hot_write_head_;
    new_chunk.filename = "/ts_chunk_" + std::to_string(new_chunk.id) + ".bin";
    
    File file = SD_MMC.open(new_chunk.filename.c_str(), FILE_WRITE);
    if (!file) {
        LOG_ERROR("Failed to open chunk file for write");
        return false;
    }
    
    size_t written = file.write(hot_buffer_, hot_write_head_);
    file.close();
    
    if (written != hot_write_head_) {
        LOG_ERROR("Failed to write chunk to SD");
        SD_MMC.remove(new_chunk.filename.c_str());
        return false;
    }
    
    chunks_.push_back(new_chunk);
    hot_write_head_ = 0;
    
    // Clean up old chunks if over max window
    while (current_download_offset_ - chunks_.front().start_offset > MAX_TS_WINDOW && !chunks_.empty()) {
        SD_MMC.remove(chunks_.front().filename.c_str());
        chunks_.erase(chunks_.begin());
    }
    
    return true;
}

size_t TimeshiftManager::read_from_cache(size_t offset, void* buffer, size_t size) {
    size_t bytes_read = 0;
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    
    // Check if in hot buffer
    size_t hot_start = current_download_offset_ - hot_write_head_;
    if (offset >= hot_start && offset < current_download_offset_) {
        size_t hot_offset = offset - hot_start;
        size_t to_read = std::min(size, hot_write_head_ - hot_offset);
        memcpy(dst, hot_buffer_ + hot_offset, to_read);
        bytes_read += to_read;
        if (bytes_read == size) return bytes_read;
        dst += to_read;
        offset += to_read;
        size -= to_read;
    }
    
    // Find chunk for remaining
    for (const auto& chunk : chunks_) {
        if (offset >= chunk.start_offset && offset < chunk.start_offset + chunk.length) {
            size_t chunk_offset = offset - chunk.start_offset;
            size_t to_read = std::min(size, chunk.length - chunk_offset);
            
            File file = SD_MMC.open(chunk.filename.c_str(), FILE_READ);
            if (!file) {
                LOG_ERROR("Failed to open chunk file for read");
                return bytes_read;
            }
            file.seek(chunk_offset);
            size_t read = file.read(dst, to_read);
            file.close();
            
            bytes_read += read;
            if (read != to_read) return bytes_read;
            if (bytes_read == size) return bytes_read;
            dst += read;
            offset += read;
            size -= read;
        }
    }
    
    return bytes_read;
}
