#include "audio_stream.h"
#include "logger.h"
#include <cstring> // for memcpy

// Frame chunk size for initialization/internal buffering if needed
static constexpr uint32_t kFramesPerChunk = 2048;

AudioStream::AudioStream() {
}

AudioStream::~AudioStream() {
    end();
}

bool AudioStream::begin(std::unique_ptr<IDataSource> source) {
    if (!source || !source->is_open()) {
        LOG_ERROR("Invalid or closed data source provided to AudioStream");
        return false;
    }
    
    source_ = std::move(source);
    
    // Init decoder
    if (!decoder_.init(source_.get(), kFramesPerChunk)) {
        LOG_ERROR("Failed to init decoder in AudioStream");
        return false;
    }
    
    if (decoder_.channels() == 0 || decoder_.sample_rate() == 0) {
        LOG_ERROR("Invalid audio format detected by decoder");
        return false;
    }

    initialized_ = true;
    return true;
}

void AudioStream::end() {
    if (initialized_) {
        decoder_.shutdown();
        if (source_) {
            source_->close();
            source_.reset();
        }
        initialized_ = false;
    }
}

size_t AudioStream::read(int16_t* buffer, size_t frames_to_read) {
    if (!initialized_) return 0;
    
    // Read directly from decoder into the provided buffer
    // Note: Mp3Decoder::read_frames expects its own internal buffer usage or we can use it directly.
    // The current Mp3Decoder implementation (based on audio_player.cpp usage) 
    // uses read_frames(int16_t *pcm_out, size_t frames_wanted).
    
    drmp3_uint64 frames_decoded = decoder_.read_frames(buffer, frames_to_read);
    return (size_t)frames_decoded;
}

bool AudioStream::seek(uint64_t pcm_frame_index) {
    if (!initialized_) return false;

    return decoder_.seek_to_frame(pcm_frame_index);
}
