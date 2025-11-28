// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include "data_source.h"
#include <LittleFS.h>
#include <Arduino.h>

class LittleFSSource : public IDataSource {
public:
    LittleFSSource() = default;
    ~LittleFSSource() override { close(); }

    bool open(const char* uri) override {
        close();
        file_ = LittleFS.open(uri, "r");
        if (file_) {
            uri_ = uri;
            size_ = file_.size();
            return true;
        }
        return false;
    }

    void close() override {
        if (file_) {
            file_.close();
        }
        uri_.clear();
        size_ = 0;
    }

    size_t read(void* buffer, size_t size) override {
        if (!file_) {
            return 0;
        }
        return file_.read(static_cast<uint8_t*>(buffer), size);
    }

    bool seek(size_t position) override {
        if (!file_) {
            return false;
        }
        return file_.seek(position);
    }

    size_t tell() const override {
        return file_ ? file_.position() : 0;
    }

    size_t size() const override {
        return size_;
    }

    bool is_open() const override {
        return file_ ? true : false;
    }

    bool is_seekable() const override {
        return true;
    }

    SourceType type() const override {
        return SourceType::LITTLEFS;
    }

    const char* uri() const override {
        return uri_.c_str();
    }

private:
    File file_;
    String uri_;
    size_t size_ = 0;
};
