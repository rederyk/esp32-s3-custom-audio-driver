#pragma once

#include "data_source.h"
#include "mp3_seek_table.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <deque>

// Forward declarations
class HTTPClient;

// Storage backend selection
enum class StorageMode {
    SD_CARD,    // Save chunks to SD card (lower memory usage, slower)
    PSRAM_ONLY  // Keep all chunks in PSRAM (faster, higher memory usage)
};

// TimeshiftManager: IDataSource intelligente che gestisce buffer circolare e cache su SD/PSRAM
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
    bool is_seekable() const override { return !ready_chunks_.empty(); } // Seekable when chunks are available
    SourceType type() const override { return SourceType::HTTP_STREAM; } // Acts as HTTP conceptually
    const char* uri() const override;
    const Mp3SeekTable* get_seek_table() const override { return &seek_table_; }

    // Timeshift specific control
    bool start();
    void stop();
    void pause_recording();
    void resume_recording();
    bool is_recording_paused() const { return pause_download_; }
    bool is_running() const { return is_running_; }

    // Storage mode control (can be changed when stream is closed)
    void setStorageMode(StorageMode mode) { storage_mode_ = mode; }
    StorageMode getStorageMode() const { return storage_mode_; }
    
    // Status info
    size_t buffered_bytes() const;
    size_t total_downloaded_bytes() const;
    float buffer_duration_seconds() const;

    // Temporal seek (NEW)
    size_t seek_to_time(uint32_t target_ms) override;     // Seek to timestamp, returns byte offset
    uint32_t total_duration_ms() const override;          // Total available duration
    uint32_t current_position_ms() const override;        // Current playback position in ms

    // Auto-pause callback for buffering (NEW)
    void set_auto_pause_callback(std::function<void(bool)> callback) { auto_pause_callback_ = callback; }
    void set_auto_pause_margin(uint32_t delay_ms, size_t min_chunks) {
        auto_pause_delay_ms_ = delay_ms;
        auto_pause_min_chunks_ = min_chunks;
    }

private:
    // ADAPTIVE BUFFER SIZING - all values computed dynamically based on detected bitrate
    static const size_t INVALID_CHUNK_ID = SIZE_MAX;
    static const uint32_t INVALID_CHUNK_ABS_ID = UINT32_MAX;

    // Bitrate detection and adaptive sizing
    uint32_t detected_bitrate_kbps_ = 0;        // Auto-detected stream bitrate
    size_t dynamic_chunk_size_ = 128 * 1024;    // Target chunk size (adaptive)
    size_t dynamic_buffer_size_ = 192 * 1024;   // Recording buffer (1.5x chunk_size)
    size_t dynamic_playback_buffer_size_ = 384 * 1024;  // Playback buffer (3x chunk_size)
    size_t dynamic_min_flush_size_ = 102 * 1024;        // Flush threshold (0.8x chunk_size)
    size_t dynamic_download_chunk_ = 4096;      // Download block size

    // Legacy constants for backward compatibility (unused, will be removed)
    static const size_t BUFFER_SIZE = 128 * 1024;
    static const size_t PLAYBACK_BUFFER_SIZE = 256 * 1024;
    static const size_t CHUNK_SIZE = 128 * 1024;
    static const size_t MAX_PSRAM_CHUNKS = 16;          // Max 16 chunks in PSRAM = 2MB

    static constexpr size_t MAX_DYNAMIC_CHUNK_BYTES = 512 * 1024;
    static constexpr size_t MAX_RECORDING_BUFFER_CAPACITY = MAX_DYNAMIC_CHUNK_BYTES + (MAX_DYNAMIC_CHUNK_BYTES / 2); // 768 KB
    static constexpr size_t MAX_PLAYBACK_BUFFER_CAPACITY = MAX_DYNAMIC_CHUNK_BYTES * 3; // 1.5 MB

    enum class ChunkState {
        PENDING,    // In scrittura su SD/PSRAM
        READY,      // Completo e disponibile per playback
        INVALID     // Errore di scrittura/validazione
    };

    struct ChunkInfo {
        uint32_t id;
        size_t start_offset;     // Offset globale di inizio
        size_t end_offset;       // Offset globale di fine
        size_t length;           // Lunghezza effettiva
        std::string filename;    // Used only in SD_CARD mode
        uint8_t* psram_ptr;      // Used only in PSRAM_ONLY mode
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
    size_t recording_buffer_capacity_ = 0;

    // PLAYBACK BUFFER (Read-Only by read() method)
    uint8_t* playback_buffer_ = nullptr;
    uint32_t current_playback_chunk_abs_id_ = UINT32_MAX;  // Absolute chunk ID (NOT index)
    size_t playback_chunk_loaded_size_ = 0;                // Size of loaded chunk
    uint32_t last_preload_check_chunk_abs_id_ = UINT32_MAX;   // Per evitare controlli di preload ripetuti
    uint32_t preloaded_chunk_abs_id_ = INVALID_CHUNK_ABS_ID;          // Chunk ID that was copied into playback_buffer_+chunk_size
    
    // Global stream state
    std::string uri_;
    bool is_open_ = false;
    bool is_running_ = false;
    StorageMode storage_mode_ = StorageMode::SD_CARD;  // Default to SD card mode

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
    size_t playback_buffer_capacity_ = 0;

    // PSRAM-only mode: circular chunk pool
    uint8_t* psram_chunk_pool_ = nullptr;    // Pre-allocated pool for PSRAM mode
    size_t psram_pool_size_ = 0;             // Total size of PSRAM pool

    // Synchronization
    SemaphoreHandle_t mutex_ = nullptr;
    bool pause_download_ = false;  // Flag to pause/resume recording

    // Auto-pause callback for buffering
    std::function<void(bool)> auto_pause_callback_ = nullptr;  // Called when buffering required
    bool is_auto_paused_ = false;  // Track if we're in auto-pause state
    uint32_t auto_pause_delay_ms_ = 1500;  // Delay before resuming (configurable)
    size_t auto_pause_min_chunks_ = 2;      // Minimum chunks needed before resuming (configurable)

    // Seek Table (built incrementally)
    Mp3SeekTable seek_table_;

    // Temporal tracking
    uint32_t cumulative_time_ms_ = 0;  // Tempo cumulativo di tutti i chunk processati

    // Bitrate monitoring helpers
    uint32_t bitrate_sample_start_ms_ = 0;
    size_t bytes_since_rate_sample_ = 0;
    std::deque<uint32_t> bitrate_history_;
    bool bitrate_adapted_once_ = false;

    // ADAPTIVE BITRATE DETECTION
    static constexpr uint32_t BITRATE_SAMPLE_WINDOW_MS = 2500;
    static constexpr size_t BITRATE_HISTORY_SIZE = 4;

    void apply_bitrate_measurement(uint32_t measured_kbps);  // Analyze throughput samples
    void calculate_adaptive_sizes(uint32_t bitrate_kbps);    // Compute buffer sizes

    // RECORDING SIDE (private helpers)
    bool flush_recording_chunk();                    // Flush recording_buffer_ to storage
    bool write_chunk_to_sd(ChunkInfo& chunk);       // Write chunk data to SD file
    bool write_chunk_to_psram(ChunkInfo& chunk);    // Write chunk data to PSRAM pool
    bool validate_chunk(ChunkInfo& chunk);          // Validate chunk integrity
    void promote_chunk_to_ready(ChunkInfo chunk);   // Move chunk from PENDING to READY
    bool calculate_chunk_duration(const ChunkInfo& chunk,
                                   uint32_t& out_frames,
                                   uint32_t& out_duration_ms,
                                   uint32_t& out_bitrate_kbps);  // Calcola durata chunk e estrae bitrate

    // PLAYBACK SIDE (private helpers)
    uint32_t find_chunk_for_offset(size_t offset);    // Find absolute chunk ID containing offset
    bool load_chunk_to_playback(uint32_t abs_chunk_id);   // Load chunk into playback_buffer_
    bool preload_next_chunk(uint32_t current_abs_chunk_id);
    size_t read_from_playback_buffer(size_t offset, void* buffer, size_t size);
    size_t find_chunk_index_by_id(uint32_t abs_chunk_id);  // Convert abs ID to array index

    // CLEANUP
    void cleanup_old_chunks();                      // Remove old chunks beyond window

    // STORAGE BACKEND HELPERS
    bool init_psram_pool();                         // Initialize PSRAM chunk pool
    void free_psram_pool();                         // Free PSRAM chunk pool
    uint8_t* allocate_psram_chunk();                // Get next available PSRAM chunk slot
    void free_chunk_storage(ChunkInfo& chunk);      // Free chunk storage (SD or PSRAM)
    
    // HTTP Handling (basic placeholder logic initially)
    // We might need a real HTTP client member here or in the task
};
