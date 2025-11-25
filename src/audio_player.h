#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <memory>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "audio_types.h"
#include "codec_es8311.h"
#include "id3_parser.h"
#include "i2s_driver.h"
#include "logger.h"
#include "mp3_decoder.h"
#include "data_source.h"
#include "data_source_littlefs.h"
#include "data_source_sdcard.h"
#include "data_source_http.h"

enum class PlayerState {
    STOPPED,
    PLAYING,
    PAUSED,
    ENDED,
    ERROR
};

enum class FailureReason {
    NONE = 0,
    RINGBUFFER_UNDERRUN,
    DECODER_INIT,
    I2S_WRITE
};

struct PlayerCallbacks {
    void (*on_start)(const char *path) = nullptr;
    void (*on_stop)(const char *path, PlayerState state) = nullptr;
    void (*on_end)(const char *path) = nullptr;
    void (*on_error)(const char *path, const char *detail) = nullptr;
    void (*on_metadata)(const Metadata &meta, const char *path) = nullptr;
};

AudioConfig default_audio_config();

class AudioPlayer {
public:
    explicit AudioPlayer(const AudioConfig &cfg = default_audio_config());

    void set_callbacks(const PlayerCallbacks &cb);
    bool select_source(const char* uri, SourceType hint = SourceType::LITTLEFS);
    bool arm_source();
    void start();
    void stop();
    void toggle_pause();
    void request_seek(int seconds);
    void set_volume(int vol_pct);
    void print_status() const;
    void handle_recovery_if_needed();
    void tick_housekeeping();

    // Info for CLI
    bool is_playing() const { return playing_; }
    PlayerState state() const { return player_state_; }
    const String &selected_path() const { return mp3_file_path_; }
    const String &armed_path() const { return armed_mp3_path_; }
    const String &active_path() const { return active_mp3_path_; }
    bool file_armed() const { return file_armed_; }
    size_t armed_file_size() const { return armed_file_size_; }
    const Metadata &metadata() const { return current_metadata_; }

    // ring buffer exposed for CLI status
    size_t ring_buffer_used() const;
    size_t ring_buffer_size() const { return pcm_ring_size_; }
    uint32_t current_sample_rate() const { return current_sample_rate_; }
    uint64_t total_frames() const { return total_pcm_frames_; }
    uint64_t played_frames() const { return current_played_frames_; }
    int current_volume() const { return current_volume_percent_; }
    int saved_volume() const { return saved_volume_percent_; }
    int user_volume() const { return user_volume_percent_; }

private:
    // Task
    static void audio_task_entry(void *param);
    BaseType_t create_task_with_affinity(TaskFunction_t task_fn,
                                         const char *name,
                                         uint32_t stack_words,
                                         void *param,
                                         UBaseType_t prio,
                                         TaskHandle_t *handle,
                                         int8_t core);

    // Helpers
    void reset_recovery_counters();
    const char *failure_reason_to_str(FailureReason reason) const;
    void schedule_recovery(FailureReason reason, const char *detail);
    BaseType_t ringbuffer_send_with_retry(RingbufHandle_t ringbuf, const void *buffer, size_t len);
    bool ringbuffer_receive_with_retry(void **item, size_t *item_size);
    void signal_task_done(EventBits_t bit);
    void wait_for_task_shutdown(uint32_t timeout_ms);
    void cleanup_ring_buffer_if_idle();
    size_t align_up(size_t value, size_t alignment) const;
    void update_producer_thresholds();
    void select_ring_buffer_size(uint32_t sample_rate_hint);
    void update_memory_min();
    void reset_memory_stats();
    bool allocate_ring_buffer_with_fallback();
    void notify_start(const char *path);
    void notify_stop(const char *path, PlayerState state);
    void notify_end(const char *path);
    void notify_error(const char *path, const char *detail);
    void notify_metadata(const Metadata &meta, const char *path);

    // Task body
    void audio_task();

    // Config/static values
    const AudioConfig cfg_;
    static constexpr uint32_t kBytesPerSample = sizeof(int16_t);
    static constexpr uint32_t kDefaultChannels = 2;

    // State
    std::unique_ptr<IDataSource> data_source_;
    String current_uri_;
    String mp3_file_path_;
    String active_mp3_path_;
    String armed_mp3_path_;
    bool file_armed_ = false;
    size_t armed_file_size_ = 0;

    // Ring buffer PCM (più piccolo del precedente MP3 buffer)
    RingbufHandle_t pcm_ring_buffer_ = NULL;
    StaticRingbuffer_t pcm_ring_struct_ = {};
    uint8_t *pcm_ring_storage_ = NULL;
    size_t pcm_ring_size_ = 0;

    uint32_t i2s_write_timeout_ms_ = 0;

    struct MemoryStats {
        size_t heap_free_start = 0;
        size_t heap_free_min = SIZE_MAX;
    } mem_stats_;

    PlayerCallbacks callbacks_;
    Metadata current_metadata_;

    volatile bool stop_requested_ = false;
    volatile bool playing_ = false;
    volatile bool pause_flag_ = false;
    volatile int seek_seconds_ = -1;
    PlayerState player_state_ = PlayerState::STOPPED;

    uint64_t total_pcm_frames_ = 0;
    uint64_t current_played_frames_ = 0;
    uint32_t current_sample_rate_ = 0;
    uint64_t mp3_file_size_ = 0;
    int saved_volume_percent_ = 0;
    int user_volume_percent_ = 0;
    int current_volume_percent_ = 0;

    FailureReason last_failure_reason_ = FailureReason::NONE;
    volatile bool recovery_scheduled_ = false;
    volatile uint32_t recovery_attempts_ = 0;

    TaskHandle_t audio_task_handle_ = NULL;
    EventGroupHandle_t playback_events_ = NULL;

    // Components
    CodecES8311 codec_;
    I2sDriver i2s_driver_;
    Id3Parser id3_parser_;
    Mp3Decoder decoder_;
};
