// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include <cstdint>
#include <memory>

// Simple effects parameters
struct EQParams {
    float bass_gain = 1.0f;
    float mid_gain = 1.0f;
    float treble_gain = 1.0f;
};

struct ReverbParams {
    float decay = 0.5f;
    float mix = 0.3f;
};

struct EchoParams {
    float delay_ms = 200.0f;
    float decay = 0.4f;
    float mix = 0.2f;
};

class EffectsChain {
public:
    EffectsChain();
    ~EffectsChain();

    // Initialize with sample rate
    void setSampleRate(uint32_t sample_rate);

    // Enable/disable effects
    void setEQEnabled(bool enabled) { eq_enabled_ = enabled; }
    void setReverbEnabled(bool enabled) { reverb_enabled_ = enabled; }
    void setEchoEnabled(bool enabled) { echo_enabled_ = enabled; }

    // Set parameters (preserves when sample rate changes)
    void setEQParams(const EQParams& params) { eq_params_ = params; }
    void setReverbParams(const ReverbParams& params) { reverb_params_ = params; }
    void setEchoParams(const EchoParams& params) { echo_params_ = params; }

    // Process stereo PCM buffer (in-place)
    void process(int16_t* buffer, size_t samples);

    // Get current params
    const EQParams& getEQParams() const { return eq_params_; }
    const ReverbParams& getReverbParams() const { return reverb_params_; }
    const EchoParams& getEchoParams() const { return echo_params_; }

    bool isEQEnabled() const { return eq_enabled_; }
    bool isReverbEnabled() const { return reverb_enabled_; }
    bool isEchoEnabled() const { return echo_enabled_; }

private:
    uint32_t sample_rate_ = 44100;

    bool eq_enabled_ = false;
    bool reverb_enabled_ = false;
    bool echo_enabled_ = false;

    EQParams eq_params_;
    ReverbParams reverb_params_;
    EchoParams echo_params_;

    // Simple delay buffers for echo/reverb
    std::unique_ptr<float[]> delay_buffer_;
    size_t delay_buffer_size_ = 0;
    size_t delay_write_pos_ = 0;

    // Simple IIR filters for EQ
    float bass_filter_state_[2] = {0.0f, 0.0f};
    float treble_filter_state_[2] = {0.0f, 0.0f};

    // Helper methods
    void applyEQ(float* left, float* right);
    void applyReverb(float* left, float* right);
    void applyEcho(float* left, float* right);
    void updateDelayBufferSize();
};
