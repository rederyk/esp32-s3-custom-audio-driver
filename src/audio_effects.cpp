// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#include "audio_effects.h"
#include <algorithm>
#include <cmath>

EffectsChain::EffectsChain() {
    updateDelayBufferSize();
}

EffectsChain::~EffectsChain() = default;

void EffectsChain::setSampleRate(uint32_t sample_rate) {
    if (sample_rate_ != sample_rate) {
        sample_rate_ = sample_rate;
        // Reset filter states when sample rate changes
        bass_filter_state_[0] = bass_filter_state_[1] = 0.0f;
        treble_filter_state_[0] = treble_filter_state_[1] = 0.0f;
        updateDelayBufferSize();
    }
}

void EffectsChain::process(int16_t* buffer, size_t samples) {
    if (!eq_enabled_ && !reverb_enabled_ && !echo_enabled_) {
        return; // No effects enabled
    }

    // Convert to float for processing
    std::unique_ptr<float[]> float_buffer(new float[samples * 2]);
    for (size_t i = 0; i < samples * 2; ++i) {
        float_buffer[i] = buffer[i] / 32768.0f;
    }

    // Process in order: EQ -> Reverb -> Echo
    if (eq_enabled_) {
        for (size_t i = 0; i < samples; ++i) {
            applyEQ(&float_buffer[i * 2], &float_buffer[i * 2 + 1]);
        }
    }

    if (reverb_enabled_) {
        for (size_t i = 0; i < samples; ++i) {
            applyReverb(&float_buffer[i * 2], &float_buffer[i * 2 + 1]);
        }
    }

    if (echo_enabled_) {
        for (size_t i = 0; i < samples; ++i) {
            applyEcho(&float_buffer[i * 2], &float_buffer[i * 2 + 1]);
        }
    }

    // Convert back to int16
    for (size_t i = 0; i < samples * 2; ++i) {
        float val = float_buffer[i] * 32767.0f;
        buffer[i] = std::max(-32768, std::min(32767, static_cast<int>(val)));
    }
}

void EffectsChain::applyEQ(float* left, float* right) {
    // Simple bass/treble EQ using basic IIR filters
    // Bass boost (low shelf)
    const float bass_alpha = 0.1f;
    float bass_l = *left * eq_params_.bass_gain + bass_filter_state_[0] * (1.0f - bass_alpha);
    bass_filter_state_[0] = bass_l * bass_alpha + bass_filter_state_[0] * (1.0f - bass_alpha);

    float bass_r = *right * eq_params_.bass_gain + bass_filter_state_[1] * (1.0f - bass_alpha);
    bass_filter_state_[1] = bass_r * bass_alpha + bass_filter_state_[1] * (1.0f - bass_alpha);

    // Treble boost (high shelf)
    const float treble_alpha = 0.05f;
    float treble_l = *left * eq_params_.treble_gain + treble_filter_state_[0] * (1.0f - treble_alpha);
    treble_filter_state_[0] = treble_l * treble_alpha + treble_filter_state_[0] * (1.0f - treble_alpha);

    float treble_r = *right * eq_params_.treble_gain + treble_filter_state_[1] * (1.0f - treble_alpha);
    treble_filter_state_[1] = treble_r * treble_alpha + treble_filter_state_[1] * (1.0f - treble_alpha);

    // Combine bass and treble, apply mid gain
    *left = (bass_l + treble_l) * 0.5f * eq_params_.mid_gain;
    *right = (bass_r + treble_r) * 0.5f * eq_params_.mid_gain;
}

void EffectsChain::applyReverb(float* left, float* right) {
    // Simple reverb using multiple delay taps
    const size_t delays[] = {23, 41, 59, 73}; // Prime numbers for diffusion
    float wet_l = 0.0f, wet_r = 0.0f;

    for (size_t tap : delays) {
        size_t pos = (delay_write_pos_ - tap + delay_buffer_size_) % delay_buffer_size_;
        wet_l += delay_buffer_[pos * 2] * reverb_params_.decay;
        wet_r += delay_buffer_[pos * 2 + 1] * reverb_params_.decay;
    }

    wet_l /= 4.0f;
    wet_r /= 4.0f;

    // Mix dry and wet
    *left = *left * (1.0f - reverb_params_.mix) + wet_l * reverb_params_.mix;
    *right = *right * (1.0f - reverb_params_.mix) + wet_r * reverb_params_.mix;

    // Store current sample in delay buffer
    delay_buffer_[delay_write_pos_ * 2] = *left;
    delay_buffer_[delay_write_pos_ * 2 + 1] = *right;
    delay_write_pos_ = (delay_write_pos_ + 1) % delay_buffer_size_;
}

void EffectsChain::applyEcho(float* left, float* right) {
    // Simple echo with single delay
    size_t delay_samples = static_cast<size_t>(echo_params_.delay_ms * sample_rate_ / 1000.0f);
    if (delay_samples >= delay_buffer_size_) delay_samples = delay_buffer_size_ - 1;

    size_t pos = (delay_write_pos_ - delay_samples + delay_buffer_size_) % delay_buffer_size_;
    float echo_l = delay_buffer_[pos * 2] * echo_params_.decay;
    float echo_r = delay_buffer_[pos * 2 + 1] * echo_params_.decay;

    // Mix dry and echo
    *left = *left * (1.0f - echo_params_.mix) + echo_l * echo_params_.mix;
    *right = *right * (1.0f - echo_params_.mix) + echo_r * echo_params_.mix;

    // Store current sample in delay buffer
    delay_buffer_[delay_write_pos_ * 2] = *left;
    delay_buffer_[delay_write_pos_ * 2 + 1] = *right;
    delay_write_pos_ = (delay_write_pos_ + 1) % delay_buffer_size_;
}

void EffectsChain::updateDelayBufferSize() {
    // Allocate buffer for maximum delay (1 second)
    size_t max_delay_samples = sample_rate_;
    delay_buffer_size_ = max_delay_samples;
    delay_buffer_.reset(new float[delay_buffer_size_ * 2]);
    std::fill_n(delay_buffer_.get(), delay_buffer_size_ * 2, 0.0f);
    delay_write_pos_ = 0;
}
