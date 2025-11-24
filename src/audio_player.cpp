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
constexpr EventBits_t FILE_TASK_DONE_BIT = BIT1;
constexpr EventBits_t PLAYBACK_TASKS_DONE = (AUDIO_TASK_DONE_BIT | FILE_TASK_DONE_BIT);
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
      ring_buffer_size_active_(cfg.ring_buffer_size_psram),
      ring_buffer_in_dram_(cfg.prefer_dram_ring),
      producer_min_free_bytes_active_(cfg.producer_min_free_bytes),
      i2s_write_timeout_ms_(cfg.i2s_write_timeout_ms),
      current_sample_rate_(cfg.default_sample_rate),
      saved_volume_percent_(cfg.default_volume_percent),
      user_volume_percent_(cfg.default_volume_percent),
      current_volume_percent_(cfg.default_volume_percent) {
    reset_memory_stats();
    update_producer_thresholds();
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

BaseType_t AudioPlayer::ringbuffer_send_with_retry(RingbufHandle_t ringbuf, const void *buffer, size_t len) {
    if (stop_requested_) {
        return pdFALSE;
    }
    for (int attempt = 0; attempt < cfg_.max_ringbuffer_retry; attempt++) {
        if (pause_flag_) {
            vTaskDelay(pdMS_TO_TICKS(20));
            attempt--;
            continue;
        }
        if (xRingbufferSend(ringbuf, buffer, len, pdMS_TO_TICKS(cfg_.ringbuffer_send_timeout_ms)) == pdTRUE) {
            return pdTRUE;
        }
        uint32_t backoff_ms = cfg_.backoff_base_ms * (1 << attempt);
        if (!pause_flag_ && !stop_requested_) {
            LOG_WARN("Ring buffer send failed, attempt %d/%d, retrying in %lu ms", attempt + 1, cfg_.max_ringbuffer_retry, backoff_ms);
        }
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }
    if (!pause_flag_ && !stop_requested_) {
        LOG_ERROR("Ring buffer send failed after %d attempts, skipping chunk", cfg_.max_ringbuffer_retry);
    }
    return pdFALSE;
}

bool AudioPlayer::ringbuffer_receive_with_retry(void **item, size_t *item_size) {
    if (stop_requested_) {
        return false;
    }
    if (file_task_handle_ == NULL) {
        size_t free_bytes = xRingbufferGetCurFreeSize(audio_ring_buffer_);
        if (free_bytes == ring_buffer_size_active_) {
            return false;
        }
    }
    for (int attempt = 0; attempt < cfg_.max_ringbuffer_retry; attempt++) {
        if (pause_flag_) {
            vTaskDelay(pdMS_TO_TICKS(20));
            attempt--;
            continue;
        }
        void *local_item = xRingbufferReceive(audio_ring_buffer_, item_size, pdMS_TO_TICKS(cfg_.ringbuffer_receive_timeout_ms));
        if (local_item != NULL) {
            *item = local_item;
            return true;
        }
        uint32_t backoff_ms = cfg_.backoff_base_ms * (1 << attempt);
        if (!pause_flag_ && !stop_requested_) {
            LOG_WARN("Ring buffer receive failed, attempt %d/%d, retrying in %lu ms", attempt + 1, cfg_.max_ringbuffer_retry, backoff_ms);
        }
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }
    if (!pause_flag_ && !stop_requested_) {
        LOG_ERROR("Ring buffer receive failed after %d attempts", cfg_.max_ringbuffer_retry);
    }
    if (!stop_requested_ && !pause_flag_) {
        schedule_recovery(FailureReason::RINGBUFFER_UNDERRUN, "ring buffer receive timeout");
    }
    return false;
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
        bool file_done = (bits & FILE_TASK_DONE_BIT) || (file_task_handle_ == NULL);
        if (audio_done && file_done) {
            if (playback_events_) {
                xEventGroupClearBits(playback_events_, PLAYBACK_TASKS_DONE);
            }
            return;
        }
        if (audio_task_handle_) {
            xTaskAbortDelay(audio_task_handle_);
        }
        if (file_task_handle_) {
            xTaskAbortDelay(file_task_handle_);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
}

void AudioPlayer::cleanup_ring_buffer_if_idle() {
    if (audio_task_handle_ == NULL && file_task_handle_ == NULL) {
        if (audio_ring_buffer_) {
            vRingbufferDelete(audio_ring_buffer_);
            audio_ring_buffer_ = NULL;
        }
        if (audio_ring_storage_) {
            heap_caps_free(audio_ring_storage_);
            audio_ring_storage_ = NULL;
        }
    }
}

size_t AudioPlayer::align_up(size_t value, size_t alignment) const {
    return (value + (alignment - 1)) & ~(alignment - 1);
}

void AudioPlayer::update_producer_thresholds() {
    producer_min_free_bytes_active_ = cfg_.producer_min_free_bytes;
    size_t soft_cap = ring_buffer_size_active_ / 3;
    if (soft_cap < (8 * 1024)) {
        soft_cap = 8 * 1024;
    }
    if (producer_min_free_bytes_active_ > soft_cap) {
        producer_min_free_bytes_active_ = soft_cap;
    }
    producer_min_free_bytes_active_ = align_up(producer_min_free_bytes_active_, 1024);

    size_t resume_margin = align_up(cfg_.producer_resume_hysteresis_min, 1024);
    if (resume_margin < producer_min_free_bytes_active_ / 4) {
        resume_margin = producer_min_free_bytes_active_ / 2;
        resume_margin = align_up(resume_margin, 1024);
    }
    producer_resume_free_bytes_active_ = producer_min_free_bytes_active_ + resume_margin;
    if (producer_resume_free_bytes_active_ > ring_buffer_size_active_) {
        producer_resume_free_bytes_active_ = ring_buffer_size_active_;
    }
    if (producer_resume_free_bytes_active_ <= producer_min_free_bytes_active_) {
        producer_resume_free_bytes_active_ = producer_min_free_bytes_active_ + 1024;
    }
}

void AudioPlayer::select_ring_buffer_size(uint32_t sample_rate_hint) {
    uint32_t sr = sample_rate_hint ? sample_rate_hint : cfg_.default_sample_rate;
    uint64_t target_bytes = (uint64_t)sr * kDefaultChannels * kBytesPerSample * cfg_.target_buffer_ms / 1000ULL;
    if (target_bytes < cfg_.ring_buffer_min_bytes) {
        target_bytes = cfg_.ring_buffer_min_bytes;
    }
    if (target_bytes > cfg_.ring_buffer_size_psram) {
        target_bytes = cfg_.ring_buffer_size_psram;
    }

    size_t capped_target = static_cast<size_t>(target_bytes);
    bool fits_dram = capped_target <= cfg_.ring_buffer_size_dram;
    ring_buffer_in_dram_ = cfg_.prefer_dram_ring && fits_dram;
    if (cfg_.prefer_dram_ring && !fits_dram) {
        LOG_WARN("Dynamic ring sizing exceeds DRAM limit (%u > %u), using PSRAM",
                 (unsigned)capped_target,
                 (unsigned)cfg_.ring_buffer_size_dram);
    }
    ring_buffer_size_active_ = align_up(capped_target, 1024);

    update_producer_thresholds();

    LOG_INFO("Dynamic ring sizing: sr=%u Hz, target %ums -> %u bytes in %s (producer min free %u bytes, resume @ %u bytes)",
             sr,
             cfg_.target_buffer_ms,
             (unsigned)ring_buffer_size_active_,
             ring_buffer_in_dram_ ? "DRAM" : "PSRAM",
             (unsigned)producer_min_free_bytes_active_,
             (unsigned)producer_resume_free_bytes_active_);
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

bool AudioPlayer::allocate_ring_buffer_with_fallback() {
    const size_t min_size = 16 * 1024;
    size_t attempt_sizes[3] = {
        ring_buffer_size_active_,
        ring_buffer_size_active_ * 3 / 4,
        ring_buffer_size_active_ / 2};

    for (size_t attempt : attempt_sizes) {
        if (attempt < min_size) {
            continue;
        }
        size_t aligned = align_up(attempt, 1024);
        uint32_t caps = ring_buffer_in_dram_ ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : MALLOC_CAP_SPIRAM;
        audio_ring_storage_ = static_cast<uint8_t *>(heap_caps_malloc(aligned, caps));
        if (!audio_ring_storage_) {
            LOG_WARN("Ring buffer alloc failed (%u bytes, %s), trying smaller",
                     (unsigned)aligned,
                     ring_buffer_in_dram_ ? "DRAM" : "PSRAM");
            continue;
        }
        audio_ring_buffer_ = xRingbufferCreateStatic(aligned, RINGBUF_TYPE_BYTEBUF, audio_ring_storage_, &audio_ring_struct_);
        if (audio_ring_buffer_) {
            ring_buffer_size_active_ = aligned;
            update_producer_thresholds();
            if (aligned != attempt_sizes[0]) {
                LOG_WARN("Ring buffer size reduced to %u bytes to fit available memory", (unsigned)aligned);
            }
            return true;
        }
        heap_caps_free(audio_ring_storage_);
        audio_ring_storage_ = NULL;
    }
    return false;
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

bool AudioPlayer::receive_ring_item(void **item, size_t *item_size) {
    if (!audio_ring_buffer_) {
        return false;
    }
    return ringbuffer_receive_with_retry(item, item_size);
}

void AudioPlayer::return_ring_item(void *item) {
    if (audio_ring_buffer_ && item) {
        vRingbufferReturnItem(audio_ring_buffer_, item);
    }
}

bool AudioPlayer::select_file(const String &path, const char *label) {
    mp3_file_path_ = path;
    file_armed_ = false;
    current_metadata_ = Metadata();
    LOG_INFO("File selezionato: %s%s", mp3_file_path_.c_str(), label ? label : "");
    return true;
}

bool AudioPlayer::arm_file() {
    File f = LittleFS.open(mp3_file_path_.c_str(), "r");
    if (!f) {
        LOG_ERROR("File non trovato: %s", mp3_file_path_.c_str());
        file_armed_ = false;
        return false;
    }
    armed_file_size_ = f.size();
    armed_mp3_path_ = mp3_file_path_;
    file_armed_ = true;
    LOG_INFO("File caricato (armed): %s, size=%u bytes", armed_mp3_path_.c_str(), (unsigned)armed_file_size_);
    f.close();
    if (id3_parser_.parse(armed_mp3_path_.c_str(), current_metadata_)) {
        const char *title = current_metadata_.title.length() ? current_metadata_.title.c_str() : "n/a";
        const char *artist = current_metadata_.artist.length() ? current_metadata_.artist.c_str() : "n/a";
        const char *album = current_metadata_.album.length() ? current_metadata_.album.c_str() : "n/a";
        LOG_INFO("Metadata: title=\"%s\" artist=\"%s\" album=\"%s\"", title, artist, album);
    } else {
        LOG_INFO("Metadata ID3 non trovati o non parsabili");
    }
    notify_metadata(current_metadata_, armed_mp3_path_.c_str());
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
    if (!audio_ring_buffer_) {
        return 0;
    }
    size_t free_bytes = xRingbufferGetCurFreeSize(audio_ring_buffer_);
    return ring_buffer_size_active_ - free_bytes;
}

void AudioPlayer::start() {
    if (player_state_ != PlayerState::STOPPED && player_state_ != PlayerState::ERROR && player_state_ != PlayerState::ENDED) {
        LOG_INFO("Already active");
        return;
    }
    if (!file_armed_) {
        LOG_WARN("Nessun file armato. Usa 'l' prima di 'p'.");
        return;
    }
    LOG_INFO("Config profile: %s", kConfigProfile);
    reset_memory_stats();
    stop_requested_ = false;
    pause_flag_ = false;
    seek_seconds_ = -1;
    current_played_frames_ = 0;
    total_pcm_frames_ = 0;
    mp3_file_size_ = 0;
    if (!playback_events_) {
        playback_events_ = xEventGroupCreate();
    }
    if (playback_events_) {
        xEventGroupClearBits(playback_events_, PLAYBACK_TASKS_DONE);
    }
    active_mp3_path_ = armed_mp3_path_;
    LOG_INFO("Playback file: %s", active_mp3_path_.c_str());
    uint32_t header_sr = 0;
    detect_mp3_bitrate_kbps(active_mp3_path_.c_str(), &header_sr);
    select_ring_buffer_size(header_sr);
    LOG_INFO("Allocating %u-byte audio ring in %s", (unsigned)ring_buffer_size_active_, ring_buffer_in_dram_ ? "DRAM" : "PSRAM");
    if (!allocate_ring_buffer_with_fallback()) {
        LOG_ERROR("Failed to allocate ring buffer in PSRAM/DRAM");
        return;
    }

    BaseType_t file_created = create_task_with_affinity(file_stream_task_entry,
                                                        "FileTask",
                                                        cfg_.file_task_stack,
                                                        this,
                                                        cfg_.file_task_priority,
                                                        &file_task_handle_,
                                                        cfg_.file_task_core);
    BaseType_t audio_created = create_task_with_affinity(audio_task_entry,
                                                         "AudioTask",
                                                         cfg_.audio_task_stack,
                                                         this,
                                                         cfg_.audio_task_priority,
                                                         &audio_task_handle_,
                                                         cfg_.audio_task_core);
    if (file_created != pdPASS || audio_created != pdPASS || file_task_handle_ == NULL || audio_task_handle_ == NULL) {
        LOG_ERROR("Failed to create tasks (audio %ld, file %ld)", (long)audio_created, (long)file_created);
        stop_requested_ = true;
        wait_for_task_shutdown(500);
        cleanup_ring_buffer_if_idle();
        playing_ = false;
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
    if (recovery_scheduled_ && !playing_ && player_state_ == PlayerState::ERROR && audio_task_handle_ == NULL && file_task_handle_ == NULL) {
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
    LOG_INFO("Ring buffer (%s) -> %u/%u bytes used", ring_buffer_in_dram_ ? "DRAM" : "PSRAM", (unsigned)ring_used, (unsigned)ring_buffer_size_active_);
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
    LOG_INFO("Tasks -> audio: %s, file: %s", audio_task_handle_ ? "alive" : "none", file_task_handle_ ? "alive" : "none");
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

void AudioPlayer::file_stream_task_entry(void *param) {
    auto *self = static_cast<AudioPlayer *>(param);
    if (self) {
        self->file_stream_task();
    }
}

void AudioPlayer::audio_task_entry(void *param) {
    auto *self = static_cast<AudioPlayer *>(param);
    if (self) {
        self->audio_task();
    }
}

void AudioPlayer::file_stream_task() {
    LOG_INFO("Task file avviato (core %d, prio %u).", (int)xPortGetCoreID(), (unsigned)uxTaskPriorityGet(NULL));

    File mp3 = LittleFS.open(active_mp3_path_.c_str(), "r");
    if (!mp3) {
        LOG_ERROR("Impossibile aprire il file MP3: %s", active_mp3_path_.c_str());
        signal_task_done(FILE_TASK_DONE_BIT);
        file_task_handle_ = NULL;
        vTaskDelete(NULL);
        return;
    }

    LOG_INFO("Riproduzione da LittleFS: %s, size=%u bytes", active_mp3_path_.c_str(), (unsigned)mp3.size());
    mp3_file_size_ = mp3.size();

    uint8_t buffer[cfg_.file_read_chunk];
    uint32_t high_watermark_hits = 0;
    size_t min_free_seen = ring_buffer_size_active_;
    while (!stop_requested_) {
        if (!audio_ring_buffer_) {
            LOG_WARN("Ring buffer non disponibile, fermo il task file.");
            break;
        }
        if (pause_flag_) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t free_bytes = xRingbufferGetCurFreeSize(audio_ring_buffer_);
        size_t used_bytes = ring_buffer_size_active_ - free_bytes;
        if (free_bytes < min_free_seen) {
            min_free_seen = free_bytes;
        }
        if (free_bytes < producer_min_free_bytes_active_) {
            high_watermark_hits++;
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        } else if (free_bytes > producer_resume_free_bytes_active_) {
            high_watermark_hits = 0;
        }
        if (free_bytes < sizeof(buffer)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int len = mp3.read(buffer, sizeof(buffer));
        if (len <= 0) {
            LOG_INFO("Fine file MP3.");
            break;
        }
        while (!stop_requested_ && ringbuffer_send_with_retry(audio_ring_buffer_, buffer, len) != pdTRUE) {
            LOG_WARN("Ring buffer pieno: ritento stesso chunk (%d bytes)", len);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    mp3.close();

    LOG_INFO("File task exit: ring buffer min free %u bytes, high watermark hits %u", (unsigned)min_free_seen, (unsigned)high_watermark_hits);

    if (stop_requested_) {
        LOG_INFO("File task stopped by user.");
    } else {
        LOG_INFO("File task terminato.");
    }
    signal_task_done(FILE_TASK_DONE_BIT);
    file_task_handle_ = NULL;
    vTaskDelete(NULL);
}

void AudioPlayer::audio_task() {
    LOG_INFO("Audio task avviato (core %d, prio %u). In attesa di dati nel buffer...",
             (int)xPortGetCoreID(),
             (unsigned)uxTaskPriorityGet(NULL));

    bool i2s_ready = false;
    const uint32_t frames_per_chunk = 2048;
    size_t min_ring_free_during_play = ring_buffer_size_active_;
    TickType_t prefill_start = 0;
    size_t leftover_capacity = ring_buffer_size_active_;

    do {
        if (!decoder_.init(this, frames_per_chunk, leftover_capacity)) {
            schedule_recovery(FailureReason::DECODER_INIT, "decoder init failed");
            break;
        }

        auto &buffers = decoder_.buffers();
        int16_t *pcm_s16 = buffers.pcm;

        prefill_start = xTaskGetTickCount();
        while (!stop_requested_ && audio_ring_buffer_ && xRingbufferGetCurFreeSize(audio_ring_buffer_) == ring_buffer_size_active_) {
            vTaskDelay(pdMS_TO_TICKS(10));
            if (((xTaskGetTickCount() - prefill_start) * portTICK_PERIOD_MS) > 1000) {
                LOG_WARN("Attesa dati ring >1s, continuo comunque");
                break;
            }
        }

        uint32_t channels = decoder_.channels();
        uint32_t sample_rate = decoder_.sample_rate();
        if (channels == 0 || sample_rate == 0) {
            schedule_recovery(FailureReason::DECODER_INIT, "decoder missing format");
            break;
        }

        LOG_INFO("Decodificatore MP3 inizializzato. Canali: %u, Sample Rate: %u", channels, sample_rate);
        total_pcm_frames_ = decoder_.total_frames();
        current_sample_rate_ = sample_rate;
        if (total_pcm_frames_ == 0 && mp3_file_size_ > 0) {
            uint32_t header_sr = 0;
            uint32_t header_bitrate_kbps = detect_mp3_bitrate_kbps(active_mp3_path_.c_str(), &header_sr);
            uint32_t sr_for_est = header_sr ? header_sr : current_sample_rate_;
            if (header_bitrate_kbps > 0) {
                total_pcm_frames_ = (mp3_file_size_ * 8ULL * sr_for_est) / (header_bitrate_kbps * 1000ULL);
                LOG_INFO("Estimated total frames: %llu (bitrate %u kbps, sr %u)", total_pcm_frames_, header_bitrate_kbps, sr_for_est);
            } else {
                uint32_t assumed_bitrate = 128 * 1024;
                total_pcm_frames_ = (mp3_file_size_ * 8ULL * current_sample_rate_) / assumed_bitrate;
                LOG_INFO("Estimated total frames: %llu (fallback 128kbps)", total_pcm_frames_);
            }
        }

        if (!codec_.init(sample_rate, kApEnable, kI2cSda, kI2cScl, kI2cSpeed, cfg_.default_volume_percent)) {
            schedule_recovery(FailureReason::DECODER_INIT, "codec init failed");
            break;
        }
        i2s_driver_.init(sample_rate, cfg_, kBytesPerSample, kDefaultChannels, kI2sBck, kI2sWs, kI2sDout);
        codec_.set_volume(user_volume_percent_);
        i2s_ready = true;

        LOG_INFO("Inizio decodifica e riproduzione...");
        while (true) {
            while (pause_flag_ && !stop_requested_) {
                vTaskDelay(pdMS_TO_TICKS(20));
                update_memory_min();
            }
            if (stop_requested_) {
                if (player_state_ != PlayerState::ERROR) {
                    player_state_ = PlayerState::STOPPED;
                }
                break;
            }
            update_memory_min();
            drmp3_uint64 frames_decoded = decoder_.read_frames(pcm_s16, frames_per_chunk);
            if (frames_decoded == 0) {
                if (stop_requested_ && player_state_ == PlayerState::ERROR) {
                    LOG_WARN("Decodificatore fermato per recovery.");
                } else if (stop_requested_) {
                    player_state_ = PlayerState::STOPPED;
                } else if (file_task_handle_ == NULL && !recovery_scheduled_) {
                    LOG_INFO("Fine decodifica (stream terminato?).");
                    player_state_ = PlayerState::ENDED;
                } else {
                    schedule_recovery(FailureReason::RINGBUFFER_UNDERRUN, "decoder got zero frames");
                }
                break;
            }
            if (stop_requested_) {
                LOG_INFO("Playback stopped by user");
                player_state_ = PlayerState::STOPPED;
                break;
            }

            current_played_frames_ += frames_decoded;

            if (seek_seconds_ >= 0) {
                uint64_t seek_frames = (uint64_t)seek_seconds_ * current_sample_rate_;
                if (seek_frames > total_pcm_frames_) seek_frames = total_pcm_frames_;
                while (current_played_frames_ < seek_frames) {
                    drmp3_uint64 discard = decoder_.read_frames(pcm_s16, 1024 / channels);
                    if (discard == 0) break;
                    current_played_frames_ += discard;
                }
                seek_seconds_ = -1;
                LOG_INFO("Seek to %llu frames", current_played_frames_);
                LOG_INFO("Seek completed to %llu seconds", seek_frames / current_sample_rate_);
            }

            if (!pause_flag_) {
                const size_t bytes_to_write = frames_decoded * channels * sizeof(int16_t);
                const uint8_t *write_ptr = reinterpret_cast<const uint8_t *>(pcm_s16);
                size_t remaining = bytes_to_write;
                uint32_t zero_writes = 0;
                size_t ring_free = audio_ring_buffer_ ? xRingbufferGetCurFreeSize(audio_ring_buffer_) : 0;
                size_t ring_used = audio_ring_buffer_ ? ring_buffer_size_active_ - ring_free : 0;
                if (ring_free < min_ring_free_during_play) {
                    min_ring_free_during_play = ring_free;
                }

                while (remaining > 0 && !stop_requested_) {
                    size_t chunk = remaining;
                    if (chunk > i2s_driver_.chunk_bytes()) {
                        chunk = i2s_driver_.chunk_bytes();
                    }
                    size_t written = 0;
                    esp_err_t result = i2s_write(I2S_NUM_0, write_ptr, chunk, &written, pdMS_TO_TICKS(cfg_.i2s_write_timeout_ms));
                    if (result != ESP_OK) {
                        LOG_ERROR("Errore scrittura I2S: %s, richiesti %u bytes (ring %u used / %u free)",
                                  esp_err_to_name(result),
                                  (unsigned)chunk,
                                  (unsigned)ring_used,
                                  (unsigned)ring_free);
                        if (!stop_requested_) {
                            schedule_recovery(FailureReason::I2S_WRITE, "i2s write failed");
                            if (player_state_ != PlayerState::ERROR) {
                                player_state_ = PlayerState::ERROR;
                            }
                        }
                        break;
                    }

                    if (written == 0) {
                        zero_writes++;
                        if (zero_writes >= 3) {
                            LOG_ERROR("I2S write returned 0 bytes per 3 tentativi consecutivi (ring %u used / %u free)", (unsigned)ring_used, (unsigned)ring_free);
                            schedule_recovery(FailureReason::I2S_WRITE, "i2s wrote zero bytes");
                            player_state_ = PlayerState::ERROR;
                            break;
                        }
                        continue;
                    }

                    if (written < chunk) {
                        LOG_WARN("I2S partial write: %u/%u bytes (ring %u used / %u free)", (unsigned)written, (unsigned)chunk, (unsigned)ring_used, (unsigned)ring_free);
                    }

                    zero_writes = 0;
                    remaining -= written;
                    write_ptr += written;
                }
            }
        }
    } while (false);

    decoder_.shutdown();
    if (i2s_ready) {
        i2s_driver_.uninstall();
    }
    PlayerState final_state = player_state_;
    String path_copy = active_mp3_path_;
    playing_ = false;
    signal_task_done(AUDIO_TASK_DONE_BIT);
    audio_task_handle_ = NULL;
    LOG_INFO("Audio task terminato. Min ring free during play: %u bytes", (unsigned)min_ring_free_during_play);
    if (final_state == PlayerState::ENDED) {
        notify_end(path_copy.c_str());
    } else if (final_state == PlayerState::ERROR) {
        notify_error(path_copy.c_str(), "audio task exit");
    }
    vTaskDelete(NULL);
}
