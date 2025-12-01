// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <memory>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "audio_types.h"
#include "audio_output.h"
#include "audio_stream.h"
#include "id3_parser.h"
#include "logger.h"
#include "data_source.h"
#include "data_source_littlefs.h"
#include "data_source_sdcard.h"
#include "data_source_http.h"
#include "audio_effects.h"

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
    void (*on_progress)(uint32_t pos_ms, uint32_t dur_ms) = nullptr;  // Progress update callback
};

AudioConfig default_audio_config();

class AudioPlayer {
public:
    explicit AudioPlayer(const AudioConfig &cfg = default_audio_config());

    void set_callbacks(const PlayerCallbacks &cb);
    bool select_source(const char* uri, SourceType hint = SourceType::LITTLEFS);
bool select_source(std::unique_ptr<IDataSource> source);
    bool arm_source();
    void start();
    void stop();
    void toggle_pause();
    void set_pause(bool pause);  // Set pause state programmatically
    void request_seek(int seconds);
    void set_volume(int vol_pct);
    void print_status() const;
    void handle_recovery_if_needed();
    void tick_housekeeping();

    // Info for CLI
    bool is_playing() const { return playing_; }
    PlayerState state() const { return player_state_; }
    const Metadata &metadata() const { return current_metadata_; }
    const IDataSource* data_source() const { 
        if (stream_) return stream_->data_source();
        return current_source_to_arm_.get(); 
    }

    // ring buffer exposed for CLI status (legacy/unused)
    size_t ring_buffer_used() const { return 0; }
    size_t ring_buffer_size() const { return 0; }
    uint32_t current_sample_rate() const { return current_sample_rate_; }
    uint64_t total_frames() const { return total_pcm_frames_; }
    uint64_t played_frames() const { return current_played_frames_; }
    int current_volume() const { return current_volume_percent_; }
    int saved_volume() const { return saved_volume_percent_; }
    int user_volume() const { return user_volume_percent_; }

    // UI Interface Methods (NEW)
    SourceType source_type() const;
    inline uint32_t current_position_ms() const {
        const IDataSource* ds = data_source();
        if (ds && ds->type() == SourceType::HTTP_STREAM) {
            // La sorgente (es. Timeshift) può riportare il tempo direttamente
            return ds->current_position_ms();
        }
        // Per file locali, calcoliamo dai frame
        if (current_sample_rate_ > 0) {
            return (current_played_frames_ * 1000) / current_sample_rate_;
        }
        return 0;
    }
    inline uint32_t total_duration_ms() const {
        const IDataSource* ds = data_source();
        if (ds && ds->type() == SourceType::HTTP_STREAM) {
            // La sorgente (es. Timeshift) può riportare la durata totale
            return ds->total_duration_ms();
        }
        // Per file locali, calcoliamo dai frame
        if (current_sample_rate_ > 0) {
            return (total_pcm_frames_ * 1000) / current_sample_rate_;
        }
        return 0;
    }

    inline uint32_t current_position_sec() const { return current_position_ms() / 1000; }
    inline uint32_t total_duration_sec() const { return total_duration_ms() / 1000; }
    const char* current_uri() const;
    uint32_t current_bitrate() const;  // Current bitrate in kbps
    AudioFormat current_format() const;  // Current audio format

    // Effects chain access
    EffectsChain& getEffectsChain() { return effects_chain_; }

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
    void signal_task_done(EventBits_t bit);
    void wait_for_task_shutdown(uint32_t timeout_ms);
    void update_memory_min();
    void reset_memory_stats();
    void notify_start(const char *path);
    void notify_stop(const char *path, PlayerState state);
    void notify_end(const char *path);
    void notify_error(const char *path, const char *detail);
    void notify_metadata(const Metadata &meta, const char *path);
    void notify_progress(uint32_t pos_ms, uint32_t dur_ms);

    // Task body
    void audio_task();

    // Config/static values
    const AudioConfig cfg_;
    static constexpr uint32_t kBytesPerSample = sizeof(int16_t);
    static constexpr uint32_t kDefaultChannels = 2;

    // State
    std::unique_ptr<IDataSource> current_source_to_arm_;
    std::unique_ptr<AudioStream> stream_;


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
    int saved_volume_percent_ = 0;
    int user_volume_percent_ = 0;
    int current_volume_percent_ = 0;

    FailureReason last_failure_reason_ = FailureReason::NONE;
    volatile bool recovery_scheduled_ = false;
    volatile uint32_t recovery_attempts_ = 0;

    TaskHandle_t audio_task_handle_ = NULL;
    EventGroupHandle_t playback_events_ = NULL;

    // Components
    AudioOutput output_;
    Id3Parser id3_parser_;
    EffectsChain effects_chain_;
};
