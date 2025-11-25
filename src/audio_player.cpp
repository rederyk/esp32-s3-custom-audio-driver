#include "audio_player.h"

#include "driver/i2s.h"
#include "esp_err.h"
#include <esp_heap_caps.h>
#include <cstring>

namespace {
// --- Pinout (invariato)
constexpr int kI2sBck = 5;
constexpr int kI2sDout = 8;
constexpr int kI2sWs = 7;
constexpr int kApEnable = 1;
constexpr int kI2cScl = 15;
constexpr int kI2cSda = 16;
constexpr uint32_t kI2cSpeed = 400000;

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
      i2s_write_timeout_ms_(cfg.i2s_write_timeout_ms),
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
        notify_error(active_mp3_path_.c_str(), detail);
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

void AudioPlayer::cleanup_ring_buffer_if_idle() {
    if (audio_task_handle_ == NULL) {
        if (pcm_ring_buffer_) {
            vRingbufferDelete(pcm_ring_buffer_);
            pcm_ring_buffer_ = NULL;
        }
        if (pcm_ring_storage_) {
            heap_caps_free(pcm_ring_storage_);
            pcm_ring_storage_ = NULL;
        }
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


uint32_t AudioPlayer::detect_mp3_bitrate_kbps(const char *path, uint32_t *header_sample_rate) {
    File f = LittleFS.open(path, "r");
    if (!f) {
        return 0;
    }

    uint8_t buf[1024];
    int len = f.read(buf, sizeof(buf));
    f.close();
    if (len < 4) return 0;

    const uint16_t bitrate_mpeg1_l3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    const uint16_t bitrate_mpeg2_l3[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    const uint32_t sr_table_mpeg1[3] = {44100, 48000, 32000};
    const uint32_t sr_table_mpeg2[3] = {22050, 24000, 16000};
    const uint32_t sr_table_mpeg25[3] = {11025, 12000, 8000};

    for (int i = 0; i <= len - 4; i++) {
        if (buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0) {
            uint8_t b1 = buf[i + 1];
            uint8_t b2 = buf[i + 2];
            int version_id = (b1 >> 3) & 0x03;
            int layer_idx = (b1 >> 1) & 0x03;
            int bitrate_idx = (b2 >> 4) & 0x0F;
            int sr_idx = (b2 >> 2) & 0x03;
            if (layer_idx != 0x01 || bitrate_idx == 0x0F || sr_idx == 0x03) {
                continue;
            }

            uint32_t sample_rate = 0;
            uint16_t bitrate_kbps = 0;
            if (version_id == 0x03) {
                sample_rate = sr_table_mpeg1[sr_idx];
                bitrate_kbps = bitrate_mpeg1_l3[bitrate_idx];
            } else if (version_id == 0x02) {
                sample_rate = sr_table_mpeg2[sr_idx];
                bitrate_kbps = bitrate_mpeg2_l3[bitrate_idx];
            } else if (version_id == 0x00) {
                sample_rate = sr_table_mpeg25[sr_idx];
                bitrate_kbps = bitrate_mpeg2_l3[bitrate_idx];
            } else {
                continue;
            }

            if (header_sample_rate) {
                *header_sample_rate = sample_rate;
            }
            return bitrate_kbps;
        }
    }
    return 0;
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
            data_source_.reset(new LittleFSSource());
            break;

        case SourceType::SD_CARD:
            data_source_.reset(new SDCardSource());
            break;

        case SourceType::HTTP_STREAM:
            data_source_.reset(new HTTPStreamSource());
            break;

        default:
            LOG_ERROR("Unknown source type: %d", (int)type);
            return false;
    }

    current_uri_ = uri;
    mp3_file_path_ = uri;
    file_armed_ = false;
    current_metadata_ = Metadata();

    LOG_INFO("Source selected: %s (type: %d)", uri, (int)type);

    return true;
}

bool AudioPlayer::arm_source() {
    if (!data_source_) {
        LOG_ERROR("No data source selected");
        return false;
    }

    if (!data_source_->open(current_uri_.c_str())) {
        LOG_ERROR("Failed to open: %s", current_uri_.c_str());
        file_armed_ = false;
        return false;
    }

    armed_file_size_ = data_source_->size();
    armed_mp3_path_ = current_uri_;
    file_armed_ = true;

    LOG_INFO("Source armed: %s, size=%u bytes, seekable=%s",
             current_uri_.c_str(),
             (unsigned)armed_file_size_,
             data_source_->is_seekable() ? "yes" : "no");

    // Parse metadata solo per file locali (non HTTP)
    if (data_source_->type() != SourceType::HTTP_STREAM) {
        if (id3_parser_.parse(current_uri_.c_str(), current_metadata_)) {
            const char *title = current_metadata_.title.length() ? current_metadata_.title.c_str() : "n/a";
            const char *artist = current_metadata_.artist.length() ? current_metadata_.artist.c_str() : "n/a";
            const char *album = current_metadata_.album.length() ? current_metadata_.album.c_str() : "n/a";
            LOG_INFO("Metadata: title=\"%s\" artist=\"%s\" album=\"%s\"", title, artist, album);
        } else {
            LOG_INFO("Metadata ID3 not found or not parseable");
        }
        notify_metadata(current_metadata_, armed_mp3_path_.c_str());
    }

    return true;
}

void AudioPlayer::set_volume(int vol_pct) {
    if (vol_pct < 0) vol_pct = 0;
    if (vol_pct > 100) vol_pct = 100;
    user_volume_percent_ = vol_pct;
    saved_volume_percent_ = vol_pct;
    codec_.set_volume(vol_pct);
    current_volume_percent_ = vol_pct;
}

void AudioPlayer::toggle_pause() {
    if (player_state_ == PlayerState::PAUSED) {
        codec_.set_volume(user_volume_percent_);
        pause_flag_ = false;
        player_state_ = PlayerState::PLAYING;
        LOG_INFO("Playback resumed");
    } else if (player_state_ == PlayerState::PLAYING) {
        pause_flag_ = true;
        codec_.set_volume(0);
        player_state_ = PlayerState::PAUSED;
        LOG_INFO("Playback paused");
    }
}

void AudioPlayer::request_seek(int seconds) {
    seek_seconds_ = seconds;
    LOG_INFO("Seek to %d seconds requested", seconds);
}

size_t AudioPlayer::ring_buffer_used() const {
    if (!pcm_ring_buffer_) {
        return 0;
    }
    size_t free_bytes = xRingbufferGetCurFreeSize(pcm_ring_buffer_);
    return pcm_ring_size_ - free_bytes;
}

void AudioPlayer::start() {
    if (player_state_ != PlayerState::STOPPED && player_state_ != PlayerState::ERROR && player_state_ != PlayerState::ENDED) {
        LOG_INFO("Already active");
        return;
    }
    if (!file_armed_ || !data_source_ || !data_source_->is_open()) {
        LOG_WARN("No source armed. Use 'l' before 'p'");
        return;
    }

    LOG_INFO("Config profile: %s", kConfigProfile);
    reset_memory_stats();

    stop_requested_ = false;
    pause_flag_ = false;
    seek_seconds_ = -1;
    current_played_frames_ = 0;
    total_pcm_frames_ = 0;

    if (!playback_events_) {
        playback_events_ = xEventGroupCreate();
    }
    if (playback_events_) {
        xEventGroupClearBits(playback_events_, AUDIO_TASK_DONE_BIT);
    }

    active_mp3_path_ = armed_mp3_path_;

    LOG_INFO("Starting playback: %s", active_mp3_path_.c_str());

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
    notify_start(active_mp3_path_.c_str());
}

void AudioPlayer::stop() {
    reset_recovery_counters();
    if (!playing_ && player_state_ == PlayerState::STOPPED) {
        LOG_INFO("Not playing.");
        return;
    }
    String stopped_path = active_mp3_path_;
    stop_requested_ = true;
    pause_flag_ = false;
    wait_for_task_shutdown(2500);
    cleanup_ring_buffer_if_idle();
    playing_ = false;
    player_state_ = PlayerState::STOPPED;
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
    cleanup_ring_buffer_if_idle();
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
    LOG_INFO("PCM ring buffer -> %u/%u bytes used", (unsigned)ring_used, (unsigned)pcm_ring_size_);
    LOG_INFO("File selected: %s | armed: %s | active: %s | armed=%s size=%u",
             mp3_file_path_.c_str(),
             armed_mp3_path_.c_str(),
             active_mp3_path_.c_str(),
             file_armed_ ? "true" : "false",
             (unsigned)armed_file_size_);
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

    if (!data_source_ || !data_source_->is_open()) {
        LOG_ERROR("DataSource not available");
        goto cleanup;
    }

    // ===== INIT DECODER =====
    if (!decoder_.init(data_source_.get(), frames_per_chunk)) {
        LOG_ERROR("Failed to init decoder");
        schedule_recovery(FailureReason::DECODER_INIT, "decoder init failed");
        goto cleanup;
    }

    channels = decoder_.channels();
    sample_rate = decoder_.sample_rate();

    if (channels == 0 || sample_rate == 0) {
        LOG_ERROR("Invalid audio format");
        schedule_recovery(FailureReason::DECODER_INIT, "invalid format");
        goto cleanup;
    }

    LOG_INFO("Decoder initialized: %u Hz, %u channels", sample_rate, channels);

    total_pcm_frames_ = decoder_.total_frames();
    current_sample_rate_ = sample_rate;

    // Stima durata se non disponibile
    if (total_pcm_frames_ == 0 && data_source_->size() > 0) {
        uint32_t header_sr = 0;
        uint32_t bitrate_kbps = detect_mp3_bitrate_kbps(current_uri_.c_str(), &header_sr);
        if (bitrate_kbps > 0) {
            total_pcm_frames_ = (data_source_->size() * 8ULL * sample_rate) / (bitrate_kbps * 1000ULL);
            LOG_INFO("Estimated duration: %llu frames (~%u seconds)",
                     total_pcm_frames_,
                     (unsigned)(total_pcm_frames_ / sample_rate));
        }
    }

    // ===== ALLOCA RING BUFFER PCM =====
    // Molto più piccolo del ring MP3: 32KB = ~180ms @ 44.1kHz stereo
    pcm_ring_size_ = 32 * 1024;
    pcm_ring_storage_ = static_cast<uint8_t*>(
        heap_caps_malloc(pcm_ring_size_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
    );

    if (!pcm_ring_storage_) {
        LOG_ERROR("Failed to allocate PCM ring buffer");
        goto cleanup;
    }

    pcm_ring_buffer_ = xRingbufferCreateStatic(
        pcm_ring_size_,
        RINGBUF_TYPE_BYTEBUF,
        pcm_ring_storage_,
        &pcm_ring_struct_
    );

    if (!pcm_ring_buffer_) {
        LOG_ERROR("Failed to create PCM ring buffer");
        goto cleanup;
    }

    LOG_INFO("PCM ring buffer: %u bytes (%.1f ms @ %u Hz)",
             (unsigned)pcm_ring_size_,
             (pcm_ring_size_ * 1000.0f) / (sample_rate * channels * sizeof(int16_t)),
             sample_rate);

    // ===== INIT CODEC & I2S =====
    if (!codec_.init(sample_rate, kApEnable, kI2cSda, kI2cScl, kI2cSpeed, cfg_.default_volume_percent)) {
        LOG_ERROR("Codec init failed");
        schedule_recovery(FailureReason::DECODER_INIT, "codec init failed");
        goto cleanup;
    }

    i2s_driver_.init(sample_rate, cfg_, kBytesPerSample, channels, kI2sBck, kI2sWs, kI2sDout);
    codec_.set_volume(user_volume_percent_);
    i2s_ready = true;

    // ===== MAIN PLAYBACK LOOP =====
    {
        auto& buffers = decoder_.buffers();
        int16_t* pcm_buffer = buffers.pcm;
        const size_t max_frames = buffers.pcm_capacity_frames;

        LOG_INFO("Starting playback loop...");

        while (!stop_requested_) {
            // PAUSE handling
            while (pause_flag_ && !stop_requested_) {
                vTaskDelay(pdMS_TO_TICKS(20));
                update_memory_min();
            }

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
                i2s_zero_dma_buffer(I2S_NUM_0);
                uint32_t after_i2s_clear_ms = millis();

                bool seek_success = decoder_.seek_to_frame(target_frame);
                uint32_t after_decoder_seek_ms = millis();

                if (seek_success) {
                    current_played_frames_ = target_frame;

                    // Svuota ring buffer PCM
                    void* item;
                    size_t size;
                    while ((item = xRingbufferReceive(pcm_ring_buffer_, &size, 0)) != NULL) {
                        vRingbufferReturnItem(pcm_ring_buffer_, item);
                    }

                    uint32_t seek_end_ms = millis();
                    uint32_t total_time = seek_end_ms - seek_start_ms;
                    uint32_t decoder_time = after_decoder_seek_ms - after_i2s_clear_ms;
                    uint32_t i2s_clear_time = after_i2s_clear_ms - seek_start_ms;

                    LOG_INFO("=== SEEK COMPLETED: Total %u ms (I2S clear: %u ms, Decoder seek: %u ms) ===",
                             total_time, i2s_clear_time, decoder_time);
                } else {
                    LOG_WARN("Native seek failed, falling back to brute force");
                    uint32_t brute_start_ms = millis();
                    // Fallback a brute force per stream non-seekable
                    while (current_played_frames_ < target_frame && !stop_requested_) {
                        drmp3_uint64 discard = decoder_.read_frames(pcm_buffer, 1024 / channels);
                        if (discard == 0) break;
                        current_played_frames_ += discard;
                    }
                    uint32_t brute_end_ms = millis();
                    LOG_INFO("=== BRUTE FORCE SEEK completed in %u ms ===", brute_end_ms - brute_start_ms);
                }

                seek_seconds_ = -1;
            }

            update_memory_min();

            // DECODE: DataSource → PCM
            drmp3_uint64 frames_decoded = decoder_.read_frames(pcm_buffer, max_frames);

            if (frames_decoded == 0) {
                if (stop_requested_) {
                    break;
                } else {
                    LOG_INFO("End of stream");
                    player_state_ = PlayerState::ENDED;
                    break;
                }
            }

            current_played_frames_ += frames_decoded;

            if (!pause_flag_) {
                // Write PCM direttamente a I2S (semplificato, senza ring intermediario)
                size_t pcm_bytes = frames_decoded * channels * sizeof(int16_t);
                const uint8_t* write_ptr = reinterpret_cast<const uint8_t*>(pcm_buffer);
                size_t remaining = pcm_bytes;

                while (remaining > 0 && !stop_requested_) {
                    size_t chunk = remaining;
                    if (chunk > i2s_driver_.chunk_bytes()) {
                        chunk = i2s_driver_.chunk_bytes();
                    }

                    size_t written = 0;
                    esp_err_t result = i2s_write(I2S_NUM_0,
                                                write_ptr,
                                                chunk,
                                                &written,
                                                pdMS_TO_TICKS(cfg_.i2s_write_timeout_ms));

                    if (result != ESP_OK) {
                        LOG_ERROR("I2S write error: %s", esp_err_to_name(result));
                        schedule_recovery(FailureReason::I2S_WRITE, "i2s write failed");
                        player_state_ = PlayerState::ERROR;
                        break;
                    }

                    if (written == 0) {
                        LOG_ERROR("I2S write returned 0 bytes");
                        schedule_recovery(FailureReason::I2S_WRITE, "i2s wrote zero bytes");
                        player_state_ = PlayerState::ERROR;
                        break;
                    }

                    remaining -= written;
                    write_ptr += written;
                }
            }
        }
    }

cleanup:
    // Cleanup
    decoder_.shutdown();

    if (pcm_ring_buffer_) {
        vRingbufferDelete(pcm_ring_buffer_);
        pcm_ring_buffer_ = NULL;
    }

    if (pcm_ring_storage_) {
        heap_caps_free(pcm_ring_storage_);
        pcm_ring_storage_ = NULL;
    }

    if (i2s_ready) {
        i2s_driver_.uninstall();
    }

    if (data_source_) {
        data_source_->close();
    }

    PlayerState final_state = player_state_;
    String path_copy = active_mp3_path_;
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
