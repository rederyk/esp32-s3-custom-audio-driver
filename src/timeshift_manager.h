#pragma once

#include "data_source.h"
#include "mp3_seek_table.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <vector>
#include <string>
#include <memory>

// Forward declarations
class HTTPClient;

// TimeshiftManager: IDataSource intelligente che gestisce buffer circolare e cache su SD
class TimeshiftManager : public IDataSource {
public:
    TimeshiftManager();
    virtual ~TimeshiftManager();

    // IDataSource interface implementation
    size_t read(void* buffer, size_t size) override;
    bool seek(size_t position) override;
    size_t tell() const override;
    size_t size() const override;
    bool open(const char* uri) override;
    void close() override;
    bool is_open() const override;
    bool is_seekable() const override { return !is_running_; } // Only seekable when download is complete
    SourceType type() const override { return SourceType::HTTP_STREAM; } // Acts as HTTP conceptually
    const char* uri() const override;
    const Mp3SeekTable* get_seek_table() const override { return &seek_table_; }

    // Timeshift specific control
    bool start();
    void stop();
    
    // Status info
    size_t buffered_bytes() const;
    size_t total_downloaded_bytes() const;
    float buffer_duration_seconds() const;

private:
    static const size_t HOT_BUFFER_SIZE = 128 * 1024;   // 128KB RAM buffer
    static const size_t CHUNK_SIZE = 512 * 1024;        // 512KB SD chunks
    
    struct ChunkInfo {
        uint32_t id;
        size_t start_offset;
        size_t length;
        std::string filename;
    };

    // Buffer in PSRAM for immediate access and write caching
    uint8_t* hot_buffer_ = nullptr;
    size_t hot_write_head_ = 0; // Where downloader writes in hot buffer (circular relative index)
    size_t hot_read_head_ = 0;  // NOT USED DIRECTLY - read depends on seek pos
    
    // Global stream state
    std::string uri_;
    bool is_open_ = false;
    bool is_running_ = false;
    
    // Download task
    TaskHandle_t download_task_handle_ = nullptr;
    static void download_task_trampoline(void* arg);
    void download_task_loop();
    
    // SD Card persistence
    std::vector<ChunkInfo> chunks_;
    size_t current_download_offset_ = 0; // Total bytes downloaded (logical end)
    size_t current_read_offset_ = 0;     // Current read position (logical head)
    
    // Synchronization
    SemaphoreHandle_t mutex_ = nullptr;
    
    // Seek Table (built incrementally)
    Mp3SeekTable seek_table_;
    
    // Internal helpers
    bool flush_to_sd(); // Move data from hot buffer to current chunk file
    size_t read_from_cache(size_t offset, void* buffer, size_t size);
    
    // HTTP Handling (basic placeholder logic initially)
    // We might need a real HTTP client member here or in the task
};
