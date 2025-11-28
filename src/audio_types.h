// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include <cstddef>
#include <cstdint>
#include "freertos/FreeRTOS.h"

struct AudioConfig {
    size_t ring_buffer_size_psram;
    size_t ring_buffer_size_dram;
    size_t ring_buffer_min_bytes;
    uint32_t target_buffer_ms;
    size_t producer_resume_hysteresis_min;
    bool prefer_dram_ring;
    uint32_t ringbuffer_send_timeout_ms;
    uint32_t ringbuffer_receive_timeout_ms;
    uint32_t max_ringbuffer_retry;
    uint32_t max_recovery_attempts;
    uint32_t backoff_base_ms;
    size_t file_read_chunk;
    size_t producer_min_free_bytes;
    uint32_t default_sample_rate;
    uint32_t audio_task_stack;
    uint32_t file_task_stack;
    UBaseType_t audio_task_priority;
    UBaseType_t file_task_priority;
    int8_t audio_task_core;
    int8_t file_task_core;
    int default_volume_percent;
    uint32_t i2s_write_timeout_ms;
    size_t i2s_chunk_bytes;
    uint32_t i2s_dma_buf_len;
    uint32_t i2s_dma_buf_count;
    bool i2s_use_apll;
};
