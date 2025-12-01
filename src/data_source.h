// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include <cstddef>
#include <cstdint>

class Mp3SeekTable;

enum class SourceType {
    LITTLEFS,
    SD_CARD,
    HTTP_STREAM
};

class IDataSource {
public:
    virtual ~IDataSource() = default;

    // Core I/O operations
    virtual size_t read(void* buffer, size_t size) = 0;
    virtual bool seek(size_t position) = 0;
    virtual size_t tell() const = 0;
    virtual size_t size() const = 0;

    // Lifecycle
    virtual bool open(const char* uri) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    // Capabilities
    virtual bool is_seekable() const = 0;
    virtual SourceType type() const = 0;
    virtual const char* uri() const = 0;
    
    // Optional: Provide a build-in seek table (e.g. for Timeshift)
    virtual const Mp3SeekTable* get_seek_table() const { return nullptr; }

    // Optional: Temporal seek for streamable sources
    virtual size_t seek_to_time(uint32_t target_ms) { return SIZE_MAX; }

    // Optional: allow cooperative stop when playback is interrupted
    virtual void request_stop() {}

    // Optional: For sources that can report temporal progress
    virtual uint32_t current_position_ms() const { return 0; }
    virtual uint32_t total_duration_ms() const { return 0; }
};
