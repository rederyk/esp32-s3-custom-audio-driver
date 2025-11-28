// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#include "audio_stream.h"
#include "audio_decoder_factory.h"
#include "logger.h"

// Frame chunk size for initialization/internal buffering if needed
static constexpr uint32_t kFramesPerChunk = 2048;

AudioStream::AudioStream() {
}

AudioStream::~AudioStream() {
    end();
}

bool AudioStream::begin(std::unique_ptr<IDataSource> source) {
    if (!source || !source->is_open()) {
        LOG_ERROR("AudioStream: Invalid or closed data source");
        return false;
    }

    // Auto-detect format and create appropriate decoder
    decoder_ = AudioDecoderFactory::create_from_source(source.get());
    if (!decoder_) {
        LOG_ERROR("AudioStream: Failed to create decoder (unknown format)");
        return false;
    }

    source_ = std::move(source);

    // Init decoder
    if (!decoder_->init(source_.get(), kFramesPerChunk)) {
        LOG_ERROR("AudioStream: Failed to init decoder");
        decoder_.reset();
        return false;
    }

    if (decoder_->channels() == 0 || decoder_->sample_rate() == 0) {
        LOG_ERROR("AudioStream: Invalid audio format detected");
        decoder_->shutdown();
        decoder_.reset();
        return false;
    }

    initialized_ = true;
    LOG_INFO("AudioStream: Initialized %s stream (%u Hz, %u ch)",
             audio_format_to_string(decoder_->format()),
             decoder_->sample_rate(),
             decoder_->channels());

    return true;
}

bool AudioStream::begin(std::unique_ptr<IDataSource> source, AudioFormat format) {
    if (!source || !source->is_open()) {
        LOG_ERROR("AudioStream: Invalid or closed data source");
        return false;
    }

    // Create decoder for explicit format
    decoder_ = AudioDecoderFactory::create(format);
    if (!decoder_) {
        LOG_ERROR("AudioStream: Failed to create decoder for format %s",
                  audio_format_to_string(format));
        return false;
    }

    source_ = std::move(source);

    // Init decoder
    if (!decoder_->init(source_.get(), kFramesPerChunk)) {
        LOG_ERROR("AudioStream: Failed to init decoder");
        decoder_.reset();
        return false;
    }

    if (decoder_->channels() == 0 || decoder_->sample_rate() == 0) {
        LOG_ERROR("AudioStream: Invalid audio format detected");
        decoder_->shutdown();
        decoder_.reset();
        return false;
    }

    initialized_ = true;
    LOG_INFO("AudioStream: Initialized %s stream (%u Hz, %u ch)",
             audio_format_to_string(decoder_->format()),
             decoder_->sample_rate(),
             decoder_->channels());

    return true;
}

void AudioStream::end() {
    if (initialized_) {
        if (decoder_) {
            decoder_->shutdown();
            decoder_.reset();
        }
        if (source_) {
            source_->close();
            source_.reset();
        }
        initialized_ = false;
    }
}

size_t AudioStream::read(int16_t* buffer, size_t frames_to_read) {
    if (!initialized_ || !decoder_) {
        return 0;
    }

    uint64_t frames_decoded = decoder_->read_frames(buffer, frames_to_read);
    return static_cast<size_t>(frames_decoded);
}

bool AudioStream::seek(uint64_t pcm_frame_index) {
    if (!initialized_ || !decoder_) {
        return false;
    }

    return decoder_->seek_to_frame(pcm_frame_index);
}

uint32_t AudioStream::sample_rate() const {
    return decoder_ ? decoder_->sample_rate() : 0;
}

uint32_t AudioStream::channels() const {
    return decoder_ ? decoder_->channels() : 0;
}

uint64_t AudioStream::total_frames() const {
    return decoder_ ? decoder_->total_frames() : 0;
}

AudioFormat AudioStream::format() const {
    return decoder_ ? decoder_->format() : AudioFormat::UNKNOWN;
}

uint32_t AudioStream::bitrate() const {
    return decoder_ ? decoder_->bitrate() : 0;
}
