// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


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
    // Mappatura esponenziale per una migliore percezione uditiva.
    // L'intervallo hardware udibile è stato identificato tra 55 e 75.
    // 0% -> 0 (muto)
    // 1-100% -> mappato esponenzialmente su [55, 75]
    if (user_pct == 0) return 0;

    // Parametri della curva logaritmica
    const double hw_min = 55.0; // Volume hardware minimo udibile
    const double hw_max = 75.0; // Volume hardware massimo desiderato

    // La formula di potenza (esponenziale) mappa l'input lineare (1-100) su una curva
    // che cresce più velocemente all'inizio e più lentamente alla fine.
    double normalized_pct = (user_pct - 1) / 99.0; // Normalizza user_pct in [0, 1]
    const double exponent = 0.5; // Esponente < 1 per una curva "veloce all'inizio"
    double scaled_pct = pow(normalized_pct, exponent);
    double hw_vol = hw_min + (hw_max - hw_min) * scaled_pct;

    return static_cast<int>(round(hw_vol));
}

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
