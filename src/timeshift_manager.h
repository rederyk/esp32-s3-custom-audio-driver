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

    // Temporal seek (NEW)
    size_t seek_to_time(uint32_t target_ms);     // Seek to timestamp, returns byte offset
    uint32_t total_duration_ms() const;          // Total available duration
    uint32_t current_position_ms() const;        // Current playback position in ms

private:
    static const size_t BUFFER_SIZE = 128 * 1024;       // 128KB recording buffer
    static const size_t PLAYBACK_BUFFER_SIZE = 256 * 1024;  // Raddoppiato a 256KB per contenere chunk corrente + successivo
    static const size_t CHUNK_SIZE = 128 * 1024;        // Manteniamo i chunk da 128KB
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

        // Temporal information
        uint32_t start_time_ms = 0;    // Timestamp inizio chunk (millisecondi)
        uint32_t duration_ms = 0;      // Durata chunk in millisecondi
        uint32_t total_frames = 0;     // Frame PCM totali nel chunk
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
    size_t last_preload_check_chunk_ = INVALID_CHUNK_ID;   // Per evitare controlli di preload ripetuti
    
    // Global stream state
    std::string uri_;
    bool is_open_ = false;
    bool is_running_ = false;
    
    // Download task
    TaskHandle_t download_task_handle_ = nullptr;
    static void download_task_trampoline(void* arg);
    void download_task_loop();

    // File Preloader Task (NEW)
    TaskHandle_t preloader_task_handle_ = nullptr;
    static void preloader_task_trampoline(void* arg);
    void preloader_task_loop();
    
    // CHUNK MANAGEMENT
    std::vector<ChunkInfo> pending_chunks_;  // Chunks being written (PENDING state)
    std::vector<ChunkInfo> ready_chunks_;    // Chunks complete and ready for playback (READY state)
    size_t current_read_offset_ = 0;         // Current read position (logical offset)
    
    // Synchronization
    SemaphoreHandle_t mutex_ = nullptr;
    bool pause_download_ = false;  // Flag to pause/resume recording

    // Seek Table (built incrementally)
    Mp3SeekTable seek_table_;

    // Temporal tracking
    uint32_t cumulative_time_ms_ = 0;  // Tempo cumulativo di tutti i chunk processati
    
    // RECORDING SIDE (private helpers)
    bool flush_recording_chunk();                    // Flush recording_buffer_ to SD as chunk
    bool write_chunk_to_sd(ChunkInfo& chunk);       // Write chunk data to SD file
    bool validate_chunk(ChunkInfo& chunk);          // Validate chunk integrity
    void promote_chunk_to_ready(ChunkInfo chunk);   // Move chunk from PENDING to READY
    bool calculate_chunk_duration(const ChunkInfo& chunk,
                                   uint32_t& out_frames,
                                   uint32_t& out_duration_ms);  // Calcola durata chunk

    // PLAYBACK SIDE (private helpers)
    size_t find_chunk_for_offset(size_t offset);    // Find chunk ID containing offset
    bool load_chunk_to_playback(size_t chunk_id);   // Load chunk into playback_buffer_
    bool preload_next_chunk(size_t current_chunk_id);
    size_t read_from_playback_buffer(size_t offset, void* buffer, size_t size);

    // CLEANUP
    void cleanup_old_chunks();                      // Remove old chunks beyond window
    
    // HTTP Handling (basic placeholder logic initially)
    // We might need a real HTTP client member here or in the task
};
