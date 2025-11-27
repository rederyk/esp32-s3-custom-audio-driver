#include "codec_es8311.h"

#include <Arduino.h>
#include <Wire.h>
#include "audio_types.h"

bool CodecES8311::init(int sample_rate,
                       int enable_pin,
                       int i2c_sda,
                       int i2c_scl,
                       uint32_t i2c_speed,
                       int default_volume_percent) {
    pinMode(enable_pin, OUTPUT);
    digitalWrite(enable_pin, LOW);

    Wire.begin(i2c_sda, i2c_scl, i2c_speed);

    es8311_handle_t es_handle = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
    if (!es_handle) {
        LOG_ERROR("es8311_create failed");
        return false;
    }

    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = false,
        .mclk_frequency = 0,
        .sample_frequency = sample_rate};

    if (es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
        LOG_ERROR("ES8311 init failed!");
        es8311_delete(es_handle);
        return false;
    }

    handle_ = es_handle;
    set_volume(default_volume_percent);
    es8311_microphone_config(handle_, false);

    LOG_INFO("ES8311 pronto.");
    return true;
}

int CodecES8311::map_user_volume_to_hw(int user_pct) {
    if (user_pct <= 0) return 0;
    if (user_pct >= 100) return 75;
    int mapped = 55 + ((user_pct - 1) * 10 + 98) / 99;
    if (mapped > 75) mapped = 75;
    return mapped;
}
//map it better with a curve 1to10%=55to65 rest 65to75=10to100
//in this way volume result udible lianearly

void CodecES8311::set_volume(int vol_pct) {
    if (vol_pct < 0) vol_pct = 0;
    if (vol_pct > 100) vol_pct = 100;
    current_volume_percent_ = vol_pct;
    if (handle_) {
        int hw_vol = map_user_volume_to_hw(vol_pct);
        es8311_voice_volume_set(handle_, hw_vol, NULL);
        LOG_INFO("Volume set to %d%% (hw %d%%)", vol_pct, hw_vol);
    }
}
