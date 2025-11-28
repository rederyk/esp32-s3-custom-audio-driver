// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include <cstdint>
#include "es8311.h"
#include "logger.h"

class CodecES8311 {
public:
    CodecES8311() = default;

    bool init(int sample_rate,
              int enable_pin,
              int i2c_sda,
              int i2c_scl,
              uint32_t i2c_speed,
              int default_volume_percent);

    void set_volume(int vol_pct);
    int current_volume() const { return current_volume_percent_; }

private:
    static int map_user_volume_to_hw(int user_pct);

    es8311_handle_t handle_ = nullptr;
    int current_volume_percent_ = 0;
};
