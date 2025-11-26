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
    void pause_recording();
    void resume_recording();
    bool is_recording_paused() const { return pause_download_; }
    
    // Status info
    size_t buffered_bytes() const;
    size_t total_downloaded_bytes() const;
    float buffer_duration_seconds() const;

private:
    static const size_t BUFFER_SIZE = 128 * 1024;       // 128KB per buffer
    static const size_t CHUNK_SIZE = 512 * 1024;        // 512KB SD chunks
    static const size_t INVALID_CHUNK_ID = SIZE_MAX;

    enum class ChunkState {
        PENDING,    // In scrittura su SD
        READY,      // Completo e disponibile per playback
        INVALID     // Errore di scrittura/validazione
    };

    struct ChunkInfo {
        uint32_t id;
        size_t start_offset;     // Offset globale di inizio
        size_t end_offset;       // Offset globale di fine
        size_t length;           // Lunghezza effettiva
        std::string filename;
        ChunkState state;
        uint32_t crc32;          // Per validazione (opzionale)
    };

    // RECORDING BUFFER (Write-Only by download task)
    uint8_t* recording_buffer_ = nullptr;
    size_t rec_write_head_ = 0;              // Write position in recording buffer (circular)
    size_t bytes_in_current_chunk_ = 0;      // Bytes accumulated for current pending chunk
    size_t current_recording_offset_ = 0;    // Total bytes recorded (global offset)
    uint32_t next_chunk_id_ = 0;             // Next chunk ID to assign

    // PLAYBACK BUFFER (Read-Only by read() method)
    uint8_t* playback_buffer_ = nullptr;
    size_t current_playback_chunk_id_ = INVALID_CHUNK_ID;  // Currently loaded chunk
    size_t playback_chunk_loaded_size_ = 0;                // Size of loaded chunk
    
    // Global stream state
    std::string uri_;
    bool is_open_ = false;
    bool is_running_ = false;
    
    // Download task
    TaskHandle_t download_task_handle_ = nullptr;
    static void download_task_trampoline(void* arg);
    void download_task_loop();
    
    // CHUNK MANAGEMENT
    std::vector<ChunkInfo> pending_chunks_;  // Chunks being written (PENDING state)
    std::vector<ChunkInfo> ready_chunks_;    // Chunks complete and ready for playback (READY state)
    size_t current_read_offset_ = 0;         // Current read position (logical offset)
    
    // Synchronization
    SemaphoreHandle_t mutex_ = nullptr;
    bool pause_download_ = false;  // Flag to pause/resume recording
    
    // Seek Table (built incrementally)
    Mp3SeekTable seek_table_;
    
    // RECORDING SIDE (private helpers)
    bool flush_recording_chunk();                    // Flush recording_buffer_ to SD as chunk
    bool write_chunk_to_sd(ChunkInfo& chunk);       // Write chunk data to SD file
    bool validate_chunk(ChunkInfo& chunk);          // Validate chunk integrity
    void promote_chunk_to_ready(ChunkInfo chunk);   // Move chunk from PENDING to READY

    // PLAYBACK SIDE (private helpers)
    size_t find_chunk_for_offset(size_t offset);    // Find chunk ID containing offset
    bool load_chunk_to_playback(size_t chunk_id);   // Load chunk into playback_buffer_
    size_t read_from_playback_buffer(size_t offset, void* buffer, size_t size);

    // CLEANUP
    void cleanup_old_chunks();                      // Remove old chunks beyond window
    
    // HTTP Handling (basic placeholder logic initially)
    // We might need a real HTTP client member here or in the task
};
