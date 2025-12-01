// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.

#include "audio_player.h"
#include "timeshift_manager.h"

#include "esp_err.h"
#include <esp_heap_caps.h>
#include <cstring>

namespace {

#ifdef AUDIO_RING_USE_DRAM
constexpr bool kPreferDramRing = true;
#else
constexpr bool kPreferDramRing = false;
#endif

#ifdef AUDIO_PRESET_LOW_MEM
constexpr const char *kConfigProfile = "LOW_MEM";
constexpr size_t kRingPsram = 64 * 1024;
constexpr size_t kRingDram = 16 * 1024;
constexpr size_t kRingMin = 12 * 1024;
constexpr uint32_t kTargetBufferMs = 250;
constexpr size_t kProducerMinFree = 12 * 1024;
constexpr size_t kFileChunk = 512;
constexpr uint32_t kAudioTaskStack = 24576;
constexpr uint32_t kFileTaskStack = 3072;
constexpr uint32_t kI2sWriteTimeout = 200;
constexpr size_t kI2sChunkBytes = 1536;
#else
constexpr const char *kConfigProfile = "DEFAULT";
constexpr size_t kRingPsram = 128 * 1024;
constexpr size_t kRingDram = 32 * 1024;
constexpr size_t kRingMin = 16 * 1024;
constexpr uint32_t kTargetBufferMs = 350;
constexpr size_t kProducerMinFree = 24 * 1024;
constexpr size_t kFileChunk = 1024;
constexpr uint32_t kAudioTaskStack = 32768;
constexpr uint32_t kFileTaskStack = 4096;
constexpr uint32_t kI2sWriteTimeout = 250;
constexpr size_t kI2sChunkBytes = 2048;
#endif

constexpr EventBits_t AUDIO_TASK_DONE_BIT = BIT0;
} // namespace

AudioConfig default_audio_config() {
    AudioConfig cfg = {
        .ring_buffer_size_psram = kRingPsram,
        .ring_buffer_size_dram = kRingDram,
        .ring_buffer_min_bytes = kRingMin,
        .target_buffer_ms = kTargetBufferMs,
        .producer_resume_hysteresis_min = 8 * 1024,
        .prefer_dram_ring = kPreferDramRing,
        .ringbuffer_send_timeout_ms = 1000,
        .ringbuffer_receive_timeout_ms = 500,
        .max_ringbuffer_retry = 5,
        .max_recovery_attempts = 3,
        .backoff_base_ms = 50,
        .file_read_chunk = kFileChunk,
        .producer_min_free_bytes = kProducerMinFree,
        .default_sample_rate = 44100,
        .audio_task_stack = kAudioTaskStack,
        .file_task_stack = kFileTaskStack,
        .audio_task_priority = 6,
        .file_task_priority = 4,
        .audio_task_core = 1,
        .file_task_core = 0,
        .default_volume_percent = 75,
        .i2s_write_timeout_ms = kI2sWriteTimeout,
        .i2s_chunk_bytes = kI2sChunkBytes,
        .i2s_dma_buf_len = 256,
        .i2s_dma_buf_count = 12,
        .i2s_use_apll = true};
    return cfg;
}

AudioPlayer::AudioPlayer(const AudioConfig &cfg)
    : cfg_(cfg),
      current_sample_rate_(cfg.default_sample_rate),
      saved_volume_percent_(cfg.default_volume_percent),
      user_volume_percent_(cfg.default_volume_percent),
      current_volume_percent_(cfg.default_volume_percent) {
    reset_memory_stats();
}

void AudioPlayer::set_callbacks(const PlayerCallbacks &cb) {
    callbacks_ = cb;
}

BaseType_t AudioPlayer::create_task_with_affinity(TaskFunction_t task_fn,
                                                  const char *name,
                                                  uint32_t stack_words,
                                                  void *param,
                                                  UBaseType_t prio,
                                                  TaskHandle_t *handle,
                                                  int8_t core) {
#if (portNUM_PROCESSORS > 1)
    if (core >= 0) {
        return xTaskCreatePinnedToCore(task_fn, name, stack_words, param, prio, handle, core);
    }
#endif
    return xTaskCreate(task_fn, name, stack_words, param, prio, handle);
}

void AudioPlayer::reset_recovery_counters() {
    recovery_attempts_ = 0;
    recovery_scheduled_ = false;
    last_failure_reason_ = FailureReason::NONE;
}

const char *AudioPlayer::failure_reason_to_str(FailureReason reason) const {
    switch (reason) {
        case FailureReason::RINGBUFFER_UNDERRUN: return "ringbuffer underrun";
        case FailureReason::DECODER_INIT: return "decoder/init failure";
        case FailureReason::I2S_WRITE: return "i2s write error";
        default: return "none";
    }
}

void AudioPlayer::schedule_recovery(FailureReason reason, const char *detail) {
    if (stop_requested_ || player_state_ == PlayerState::STOPPED) {
        return;
    }
    if (recovery_attempts_ >= cfg_.max_recovery_attempts) {
        player_state_ = PlayerState::ERROR;
        recovery_scheduled_ = false;
        stop_requested_ = true;
        LOG_ERROR("Auto-recovery limit reached, remaining in ERROR (%s)", detail);
        return;
    }
    if (!recovery_scheduled_) {
        recovery_attempts_++;
        recovery_scheduled_ = true;
        last_failure_reason_ = reason;
        player_state_ = PlayerState::ERROR;
        LOG_WARN("Scheduling auto recovery (%s). Attempt %u/%u", detail, recovery_attempts_, cfg_.max_recovery_attempts);
        const char* uri = "n/a";
        if (stream_ && stream_->data_source()) uri = stream_->data_source()->uri();
        else if (current_source_to_arm_) uri = current_source_to_arm_->uri();
        notify_error(uri, detail);
    }
    stop_requested_ = true;
}


void AudioPlayer::signal_task_done(EventBits_t bit) {
    if (playback_events_) {
        xEventGroupSetBits(playback_events_, bit);
    }
}

void AudioPlayer::wait_for_task_shutdown(uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        EventBits_t bits = playback_events_ ? xEventGroupGetBits(playback_events_) : 0;
        bool audio_done = (bits & AUDIO_TASK_DONE_BIT) || (audio_task_handle_ == NULL);
        if (audio_done) {
            if (playback_events_) {
                xEventGroupClearBits(playback_events_, AUDIO_TASK_DONE_BIT);
            }
            return;
        }
        if (audio_task_handle_) {
            xTaskAbortDelay(audio_task_handle_);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
}



void AudioPlayer::reset_memory_stats() {
    mem_stats_.heap_free_start = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    mem_stats_.heap_free_min = mem_stats_.heap_free_start;
}

void AudioPlayer::update_memory_min() {
    size_t cur = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (cur < mem_stats_.heap_free_min) {
        mem_stats_.heap_free_min = cur;
    }
}

void AudioPlayer::notify_start(const char *path) {
    if (callbacks_.on_start) {
        callbacks_.on_start(path);
    }
}

void AudioPlayer::notify_stop(const char *path, PlayerState state) {
    if (callbacks_.on_stop) {
        callbacks_.on_stop(path, state);
    }
}

void AudioPlayer::notify_end(const char *path) {
    if (callbacks_.on_end) {
        callbacks_.on_end(path);
    }
}

void AudioPlayer::notify_error(const char *path, const char *detail) {
    if (callbacks_.on_error) {
        callbacks_.on_error(path, detail);
    }
}

void AudioPlayer::notify_metadata(const Metadata &meta, const char *path) {
    if (callbacks_.on_metadata) {
        callbacks_.on_metadata(meta, path);
    }
}

void AudioPlayer::notify_progress(uint32_t pos_ms, uint32_t dur_ms) {
    if (callbacks_.on_progress) {
        callbacks_.on_progress(pos_ms, dur_ms);
    }
}

uint32_t AudioPlayer::current_bitrate() const {
    return stream_ ? stream_->bitrate() : 0;
}

SourceType AudioPlayer::source_type() const {
    const IDataSource* ds = data_source();
    return ds ? ds->type() : SourceType::LITTLEFS;
}

const char* AudioPlayer::current_uri() const {
    const IDataSource* ds = data_source();
    return ds ? ds->uri() : "";
}

AudioFormat AudioPlayer::current_format() const {
    return stream_ ? stream_->format() : AudioFormat::UNKNOWN;
}

bool AudioPlayer::select_source(const char* uri, SourceType hint) {
    // Auto-detect da URI se hint è LITTLEFS (default)
    SourceType type = hint;

    if (hint == SourceType::LITTLEFS) {
        // Euristica per auto-detect
        if (strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0) {
            type = SourceType::HTTP_STREAM;
        } else if (strncmp(uri, "/sd/", 4) == 0) {
            type = SourceType::SD_CARD;
        } else {
            type = SourceType::LITTLEFS;
        }
    }

    // Crea DataSource appropriata
    switch (type) {
        case SourceType::LITTLEFS:
            current_source_to_arm_.reset(new LittleFSSource());
            break;

        case SourceType::SD_CARD:
            current_source_to_arm_.reset(new SDCardSource());
            break;

        case SourceType::HTTP_STREAM:
            current_source_to_arm_.reset(new TimeshiftManager());
            break;

        default:
            LOG_ERROR("Unknown source type: %d", (int)type);
            return false;
    }

    current_metadata_ = Metadata();

    LOG_INFO("Source selected: %s (type: %d)", uri, (int)type);
    return current_source_to_arm_->open(uri);
}

bool AudioPlayer::select_source(std::unique_ptr<IDataSource> source) {
    if (!source) {
        return false;
    }
    current_source_to_arm_ = std::move(source);
    current_metadata_ = Metadata();
    return true;
}

bool AudioPlayer::arm_source() {
    if (!current_source_to_arm_) {
        LOG_ERROR("No data source selected");
        return false;
    }

    // Se la sorgente non è già aperta, prova ad aprirla.
    // Questo rende 'arm_source' idempotente.
    if (!current_source_to_arm_->is_open()) {
        if (!current_source_to_arm_->open(current_source_to_arm_->uri())) {
            LOG_ERROR("Failed to open: %s", current_source_to_arm_->uri());
            return false;
        }
    }

    LOG_INFO("Source armed: %s, size=%u bytes, seekable=%s",
             current_source_to_arm_->uri(),
             (unsigned)current_source_to_arm_->size(),
             current_source_to_arm_->is_seekable() ? "yes" : "no");

    if (current_source_to_arm_->is_seekable()) {
        if (id3_parser_.parse(current_source_to_arm_.get(), current_metadata_)) {
            LOG_INFO("Metadata: title=\"%s\" artist=\"%s\" album=\"%s\"", current_metadata_.title.c_str(), current_metadata_.artist.c_str(), current_metadata_.album.c_str());
        } else {
            LOG_INFO("Metadata ID3 not found or not parseable");
        }
        notify_metadata(current_metadata_, current_source_to_arm_->uri());
    }

    return true;
}

void AudioPlayer::set_volume(int vol_pct) {
    if (vol_pct < 0) vol_pct = 0;
    if (vol_pct > 100) vol_pct = 100;
    user_volume_percent_ = vol_pct;
    saved_volume_percent_ = vol_pct;
    output_.set_volume(vol_pct);
    current_volume_percent_ = vol_pct;
}

void AudioPlayer::toggle_pause() {
    if (player_state_ == PlayerState::PAUSED) {
        output_.set_volume(user_volume_percent_);
        pause_flag_ = false;
        player_state_ = PlayerState::PLAYING;



        LOG_INFO("Playback resumed");
    } else if (player_state_ == PlayerState::PLAYING) {
        pause_flag_ = true;
        output_.set_volume(0);
        player_state_ = PlayerState::PAUSED;



        LOG_INFO("Playback paused");
    }
}

void AudioPlayer::set_pause(bool pause) {
    if (pause && player_state_ == PlayerState::PLAYING) {
        // Pause playback
        pause_flag_ = true;
        output_.set_volume(0);
        player_state_ = PlayerState::PAUSED;



        LOG_INFO("Playback paused");
    } else if (!pause && player_state_ == PlayerState::PAUSED) {
        // Resume playback
        output_.set_volume(user_volume_percent_);
        pause_flag_ = false;
        player_state_ = PlayerState::PLAYING;

        LOG_INFO("Playback resumed");
    }
}

void AudioPlayer::request_seek(int seconds) {
    seek_seconds_ = seconds;
    LOG_INFO("Seek to %d seconds requested", seconds);
}


void AudioPlayer::start() {
    if (player_state_ != PlayerState::STOPPED && player_state_ != PlayerState::ERROR && player_state_ != PlayerState::ENDED) {
        LOG_INFO("Already active");
        return;
    }
    if (!current_source_to_arm_ || !current_source_to_arm_->is_open()) {
        LOG_WARN("No source armed. Use 'l' before 'p'");
        return;
    }

    LOG_INFO("Config profile: %s", kConfigProfile);
    reset_memory_stats();

    stream_.reset(new AudioStream());
    if (!stream_->begin(std::move(current_source_to_arm_))) {
        LOG_ERROR("Failed to begin stream");
        player_state_ = PlayerState::ERROR;
        stream_.reset();
        return;
    }

    stop_requested_ = false;
    pause_flag_ = false;
    seek_seconds_ = -1;
    current_played_frames_ = 0;
    total_pcm_frames_ = stream_->total_frames();
    current_sample_rate_ = stream_->sample_rate();
    effects_chain_.setSampleRate(current_sample_rate_);

    if (!playback_events_) {
        playback_events_ = xEventGroupCreate();
    }
    if (playback_events_) {
        xEventGroupClearBits(playback_events_, AUDIO_TASK_DONE_BIT);
    }

    const char* uri = stream_->data_source()->uri();
    LOG_INFO("Starting playback: %s", uri);

    // Crea SOLO audio task (no file task!)
    BaseType_t created = create_task_with_affinity(
        audio_task_entry,
        "AudioTask",
        cfg_.audio_task_stack,
        this,
        cfg_.audio_task_priority,
        &audio_task_handle_,
        cfg_.audio_task_core
    );

    if (created != pdPASS || audio_task_handle_ == NULL) {
        LOG_ERROR("Failed to create audio task");
        player_state_ = PlayerState::ERROR;
        return;
    }

    playing_ = true;
    player_state_ = PlayerState::PLAYING;

    LOG_INFO("Playback started");
    notify_start(uri);
}

void AudioPlayer::stop() {
    reset_recovery_counters();
    if (!playing_ && player_state_ == PlayerState::STOPPED) {
        LOG_INFO("Not playing.");
        return;
    }
    String stopped_path = (stream_ && stream_->data_source()) ? stream_->data_source()->uri() : "";
    const IDataSource* ds = (stream_) ? stream_->data_source() : nullptr;
    if (ds) {
        // Cooperative stop so blocking sources (e.g. timeshift) can exit immediately
        const_cast<IDataSource*>(ds)->request_stop();
    }
    stop_requested_ = true;
    pause_flag_ = false;
    wait_for_task_shutdown(2500);
    playing_ = false;
    player_state_ = PlayerState::STOPPED;
    
    // Clean up stream
    stream_.reset();

    size_t heap_end = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    LOG_INFO("Playback stopped. Heap delta: start %u -> min %u -> end %u (diff %d)",
             (unsigned)mem_stats_.heap_free_start,
             (unsigned)mem_stats_.heap_free_min,
             (unsigned)heap_end,
             (int)(mem_stats_.heap_free_start - heap_end));
    notify_stop(stopped_path.c_str(), player_state_);
}

void AudioPlayer::handle_recovery_if_needed() {
    if (recovery_scheduled_ && !playing_ && player_state_ == PlayerState::ERROR && audio_task_handle_ == NULL) {
        LOG_INFO("Auto recovery attempt %u/%u after %s", recovery_attempts_, cfg_.max_recovery_attempts, failure_reason_to_str(last_failure_reason_));
        recovery_scheduled_ = false;
        player_state_ = PlayerState::STOPPED;
        stop_requested_ = false;
        start();
    }
}

void AudioPlayer::tick_housekeeping() {
    update_memory_min();
    handle_recovery_if_needed();
}

void AudioPlayer::print_status() const {
    const char* state_str = nullptr;
    switch (player_state_) {
        case PlayerState::STOPPED: state_str = "STOPPED"; break;
        case PlayerState::PLAYING: state_str = "PLAYING"; break;
        case PlayerState::PAUSED:  state_str = "PAUSED";  break;
        case PlayerState::ENDED:   state_str = "ENDED";   break;
        case PlayerState::ERROR:   state_str = "ERROR";   break;
        default:      state_str = "UNKNOWN"; break;
    }

    size_t ring_used = ring_buffer_used();

    LOG_INFO("=== Player Status ===");
    LOG_INFO("State: %s", state_str);
    LOG_INFO("Volume: %d%% (saved: %d%%)", current_volume_percent_, saved_volume_percent_);
    LOG_INFO("Sample Rate: %u Hz", current_sample_rate_);
    const IDataSource* src = data_source();
    if (src) {
        LOG_INFO("Source: %s | open: %s | size: %u bytes",
                 src->uri(),
                 src->is_open() ? "yes" : "no",
                 (unsigned)src->size());
    } else {
        LOG_INFO("Source: not selected");
    }
    const char *title = current_metadata_.title.length() ? current_metadata_.title.c_str() : "n/a";
    const char *artist = current_metadata_.artist.length() ? current_metadata_.artist.c_str() : "n/a";
    const char *album = current_metadata_.album.length() ? current_metadata_.album.c_str() : "n/a";
    const char *genre = current_metadata_.genre.length() ? current_metadata_.genre.c_str() : "n/a";
    const char *track = current_metadata_.track.length() ? current_metadata_.track.c_str() : "n/a";
    const char *year = current_metadata_.year.length() ? current_metadata_.year.c_str() : "n/a";
    const char *comment = current_metadata_.comment.length() ? current_metadata_.comment.c_str() : "n/a";
    const char *custom = current_metadata_.custom.length() ? current_metadata_.custom.c_str() : "n/a";
    LOG_INFO("Metadata: title=\"%s\" artist=\"%s\" album=\"%s\" genre=\"%s\" track=\"%s\" year=\"%s\" cover=%s", title, artist, album, genre, track, year, current_metadata_.cover_present ? "yes" : "no");
    LOG_INFO("Metadata extra: comment=\"%s\" custom=\"%s\"", comment, custom);
    LOG_INFO("Task -> audio: %s", audio_task_handle_ ? "alive" : "none");
    LOG_INFO("Frames played: %llu / %llu", current_played_frames_, total_pcm_frames_);
    LOG_INFO("Stop flag: %s, Pause flag: %s", stop_requested_ ? "true" : "false", pause_flag_ ? "true" : "false");
    LOG_INFO("Recovery: %s (reason: %s) attempts %u/%u",
             recovery_scheduled_ ? "scheduled" : "idle",
             failure_reason_to_str(last_failure_reason_),
             recovery_attempts_,
             cfg_.max_recovery_attempts);
    LOG_INFO("Heap monitor -> start %u, min %u, current %u",
             (unsigned)mem_stats_.heap_free_start,
             (unsigned)mem_stats_.heap_free_min,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    LOG_INFO("=====================");
}

void AudioPlayer::audio_task_entry(void *param) {
    auto *self = static_cast<AudioPlayer *>(param);
    if (self) {
        self->audio_task();
    }
}

void AudioPlayer::audio_task() {
    LOG_INFO("Audio task started (core %d)", (int)xPortGetCoreID());

    // Declare all variables before goto to avoid crossing initialization
    bool i2s_ready = false;
    const uint32_t frames_per_chunk = 2048;
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    int16_t* pcm_buffer = nullptr;
    size_t pcm_buffer_size_frames = 2048;
    uint32_t last_progress_update_ms = 0;
    static constexpr uint32_t kProgressUpdateIntervalMs = 250;  // Update every 250ms

    // Check is done below with stream check

    // Stream is already initialized in start()
    if (!stream_) {
        LOG_ERROR("Stream not initialized");
        goto cleanup;
    }

    channels = stream_->channels();
    sample_rate = stream_->sample_rate();
    // total_pcm_frames_ is updated in start()

    // ===== INIT AUDIO OUTPUT (Codec & I2S) =====
    if (!output_.begin(cfg_, sample_rate, channels)) {
        LOG_ERROR("Audio output init failed");
        schedule_recovery(FailureReason::DECODER_INIT, "output init failed");
        goto cleanup;
    }
    output_.set_volume(user_volume_percent_);
    i2s_ready = true;

    // ===== ALLOCATE TEMP PCM BUFFER =====
    // Using a heap buffer for PCM data
    pcm_buffer = (int16_t*)heap_caps_malloc(pcm_buffer_size_frames * channels * sizeof(int16_t), MALLOC_CAP_8BIT);
    if (!pcm_buffer) {
        LOG_ERROR("Failed to allocate PCM buffer");
        goto cleanup;
    }

    // ===== MAIN PLAYBACK LOOP =====
    LOG_INFO("Starting playback loop...");

    while (!stop_requested_) {
        // PAUSE handling
        while (pause_flag_ && !stop_requested_) {
                vTaskDelay(pdMS_TO_TICKS(20));
                update_memory_min();
            }
        if (pause_flag_) {
            vTaskDelay(pdMS_TO_TICKS(50));
            update_memory_min();
            continue; // Salta il resto del loop e ricontrolla le flag
        }

            if (stop_requested_) break;
        if (stop_requested_) break;

            // SEEK handling - ORA FUNZIONA ANCHE IN PAUSA!
            if (seek_seconds_ >= 0) {
                uint64_t target_frame = (uint64_t)seek_seconds_ * sample_rate;
                if (target_frame > total_pcm_frames_) {
                    target_frame = total_pcm_frames_;
                }

                uint32_t seek_start_ms = millis();
                uint64_t seek_distance = (target_frame > current_played_frames_)
                    ? (target_frame - current_played_frames_)
                    : (current_played_frames_ - target_frame);

                LOG_INFO("=== SEEK START: from frame %llu to %llu (distance: %llu frames, %u sec) ===",
                         current_played_frames_, target_frame, seek_distance, seek_seconds_);

                // Pulisci buffer DMA I2S per evitare suoni ripetuti durante seek
                output_.stop();
                uint32_t after_i2s_clear_ms = millis();

                bool seek_success = false;

                // Prova il seek temporale se la sorgente lo supporta (es. TimeshiftManager)
                IDataSource* ds_nc = const_cast<IDataSource*>(stream_->data_source());
                if (ds_nc) {
                    uint32_t target_ms = (uint32_t)seek_seconds_ * 1000;
                    size_t byte_offset = ds_nc->seek_to_time(target_ms);

                    if (byte_offset != SIZE_MAX) {
                        // La sorgente supporta il seek temporale - usa il byte offset ritornato!
                        LOG_INFO("Temporal seek to %u ms → byte offset %u", target_ms, (unsigned)byte_offset);

                        // Seek diretto sulla datasource - NON chiamare stream_->seek(0)!
                        // Il decoder ricomincerà automaticamente a leggere dal nuovo offset
                        if (ds_nc->seek(byte_offset)) {
                            // Il decoder è ora posizionato all'inizio del chunk seekato
                            current_played_frames_ = target_frame;
                            seek_success = true;
                            LOG_INFO("Temporal seek successful");
                        } else {
                            LOG_WARN("Byte offset seek failed, trying frame seek");
                            seek_success = stream_->seek(target_frame);
                        }
                    } else {
                        // Seek temporale non supportato, usa seek a frame standard
                        seek_success = stream_->seek(target_frame);
                    }
                } else {
                    // Use standard frame-based seek for other sources
                    seek_success = stream_->seek(target_frame);
                }

                uint32_t after_decoder_seek_ms = millis();

                if (seek_success) {
                    uint32_t seek_end_ms = millis();
                    uint32_t total_time = seek_end_ms - seek_start_ms;
                    uint32_t decoder_time = after_decoder_seek_ms - after_i2s_clear_ms;
                    uint32_t i2s_clear_time = after_i2s_clear_ms - seek_start_ms;

                    LOG_INFO("=== SEEK COMPLETED: Total %u ms (I2S clear: %u ms, Decoder seek: %u ms) ===",
                             total_time, i2s_clear_time, decoder_time);
                    
                    // CRITICAL FIX: Always update the frame counter after a successful seek
                    // to prevent state desynchronization.
                    current_played_frames_ = target_frame;
                } else {
                    LOG_WARN("Native seek failed, falling back to brute force");
                    uint32_t brute_start_ms = millis();
                    // Fallback a brute force per stream non-seekable
                    while (current_played_frames_ < target_frame && !stop_requested_) {
                         // Read small chunks to discard
                        size_t frames_to_discard = 1024 / channels; // arbitrary small chunk
                        size_t discard = stream_->read(pcm_buffer, frames_to_discard);
                        if (discard == 0) break;
                        current_played_frames_ += discard;
                    }
                    uint32_t brute_end_ms = millis();
                    LOG_INFO("=== BRUTE FORCE SEEK completed in %u ms ===", brute_end_ms - brute_start_ms);
                }

                seek_seconds_ = -1;
            }

            update_memory_min();

            // Se un seek è stato appena eseguito, salta direttamente alla prossima iterazione
            // per leggere i dati dalla nuova posizione, invece di entrare nel blocco 'no data'.
         //   if (seek_seconds_ == -1 && stream_->read_ptr_updated_by_seek()) {
         //       // non fare nulla, il seek è stato gestito, continua al prossimo read
         //   }
            // DECODE: DataSource → PCM
            size_t frames_decoded = stream_->read(pcm_buffer, pcm_buffer_size_frames);

            if (frames_decoded == 0) {
                if (stop_requested_) {
                    break;
                }

                // For live streams (timeshift), don't immediately end - wait for new chunks
                IDataSource* ds = const_cast<IDataSource*>(stream_->data_source());
                if (ds && ds->type() == SourceType::HTTP_STREAM) {
                    // This is a live stream - check if it's still downloading
                    TimeshiftManager* ts = static_cast<TimeshiftManager*>(ds);
                    
                    // If download is still running, wait for new chunks instead of ending
                    if (ts && ts->is_running()) {
                       // LOG_DEBUG("Live stream: no data available, waiting for next chunk...");
                        // Use a shorter, more responsive delay to avoid getting stuck.
                        // This allows the task to yield and quickly re-check for data,
                        // making it more resilient to temporary buffer underruns in live streams.
                     //   vTaskDelay(pdMS_TO_TICKS(50));
                        continue; // Re-enter the loop to try reading again
                    } else if (ts) {
                        LOG_INFO("Live stream download has stopped. Ending playback.");
                        // If the timeshift is no longer running, it's the end of the stream.
                    }
                }

                // For non-live streams or when download has stopped, this is end of stream
                LOG_INFO("End of stream");
                player_state_ = PlayerState::ENDED;
                break;
            }

            current_played_frames_ += frames_decoded;

            // Progress callback (every 250ms)
            uint32_t now = millis();
            if (now - last_progress_update_ms >= kProgressUpdateIntervalMs) {
                uint32_t pos_ms = current_position_ms();
                uint32_t dur_ms = total_duration_ms();
                notify_progress(pos_ms, dur_ms);
                last_progress_update_ms = now;
            }

            if (!pause_flag_) {
                // Apply effects chain
                effects_chain_.process(pcm_buffer, frames_decoded);

                size_t frames_written = output_.write(pcm_buffer, frames_decoded, channels);
                if (frames_written < frames_decoded) {
                     // Handle partial write or error if needed, but AudioOutput logs errors.
                     // For now, we continue or could count dropped frames.
                }
            }
        } // end while (!stop_requested_)

    if (pcm_buffer) {
        heap_caps_free(pcm_buffer);
        pcm_buffer = NULL;
    }

cleanup:
    // Stream shutdown is handled by AudioPlayer::stop when resetting stream_
    // But we can end output here.
    if (i2s_ready) {
        output_.end();
    }

    PlayerState final_state = player_state_;
    String path_copy = (stream_ && stream_->data_source()) ? stream_->data_source()->uri() : "";
    playing_ = false;
    signal_task_done(AUDIO_TASK_DONE_BIT);
    audio_task_handle_ = NULL;

    LOG_INFO("Audio task terminated");

    if (final_state == PlayerState::ENDED) {
        notify_end(path_copy.c_str());
    } else if (final_state == PlayerState::ERROR) {
        notify_error(path_copy.c_str(), "audio task exit");
    }

    vTaskDelete(NULL);
}
