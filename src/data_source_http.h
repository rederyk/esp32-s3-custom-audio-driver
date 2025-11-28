// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include "data_source.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <Arduino.h>
#include "logger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class HTTPStreamSource : public IDataSource {
public:
    HTTPStreamSource() = default;
    ~HTTPStreamSource() override { close(); }

    bool open(const char* uri) override {
        close();
        url_ = uri;

        // Prima richiesta HEAD per controllare Range support e content length
        http_.begin(uri);
        http_.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

        int code = http_.sendRequest("HEAD");
        if (code < 0) {
            LOG_ERROR("HTTP HEAD failed: %s", http_.errorToString(code).c_str());
            http_.end();
            return false;
        }

        // Check Range support
        if (http_.hasHeader("Accept-Ranges")) {
            String ranges = http_.header("Accept-Ranges");
            supports_range_ = (ranges.indexOf("bytes") >= 0);
            LOG_INFO("Server supports Range: %s", supports_range_ ? "YES" : "NO");
        }

        // Get content length
        if (http_.hasHeader("Content-Length")) {
            content_length_ = http_.header("Content-Length").toInt();
        }

        http_.end();

        // Ora GET per aprire stream
        if (!reconnect(0)) {
            return false;
        }

        return true;
    }

    void close() override {
        if (stream_) {
            stream_->stop();
            stream_ = nullptr;
        }
        http_.end();
        url_.clear();
        content_length_ = 0;
        position_ = 0;
        buffer_fill_ = 0;
        buffer_pos_ = 0;
        supports_range_ = false;
    }

    size_t read(void* buffer, size_t size) override {
        uint8_t* dst = static_cast<uint8_t*>(buffer);
        size_t total_read = 0;
        int retry_count = 0;
        const int max_retry = 3;

        while (total_read < size && retry_count < max_retry) {
            // 1. Svuota buffer locale se disponibile
            if (buffer_pos_ < buffer_fill_) {
                size_t avail = buffer_fill_ - buffer_pos_;
                size_t to_copy = (avail < (size - total_read)) ? avail : (size - total_read);
                memcpy(dst + total_read, local_buffer_ + buffer_pos_, to_copy);
                buffer_pos_ += to_copy;
                total_read += to_copy;
                position_ += to_copy;
                continue;
            }

            // 2. Ricarica da stream HTTP provando a saturare la richiesta
            if (!stream_ || !stream_->connected()) {
                LOG_WARN("HTTP stream disconnected, reconnecting (%d/%d)",
                         retry_count + 1, max_retry);
                if (reconnect(position_)) {
                    retry_count++;
                    continue;
                }
                break;
            }

            size_t to_read = (size - total_read > sizeof(local_buffer_))
                                 ? sizeof(local_buffer_)
                                 : (size - total_read);

            // readBytes blocca fino a timeout per ottenere tutti i bytes richiesti
            buffer_fill_ = stream_->readBytes(local_buffer_, to_read);
            buffer_pos_ = 0;

            if (buffer_fill_ == 0) {
                LOG_WARN("HTTP read returned 0 bytes");
                retry_count++;
                continue;
            }
        }

        return total_read;
    }

    bool seek(size_t position) override {
        if (!supports_range_) {
            LOG_WARN("HTTP server does not support Range requests");
            return false;
        }

        return reconnect(position);
    }

    size_t tell() const override {
        return position_;
    }

    size_t size() const override {
        return content_length_;
    }

    bool is_open() const override {
        return stream_ != nullptr && stream_->connected();
    }

    bool is_seekable() const override {
        return supports_range_;
    }

    SourceType type() const override {
        return SourceType::HTTP_STREAM;
    }

    const char* uri() const override {
        return url_.c_str();
    }

private:
    bool reconnect(size_t from_position) {
        if (stream_) {
            stream_->stop();
            stream_ = nullptr;
        }
        http_.end();

        http_.begin(url_);
        http_.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http_.setTimeout(10000);  // 10s timeout

        if (from_position > 0 && supports_range_) {
            String range = "bytes=" + String((unsigned long)from_position) + "-";
            http_.addHeader("Range", range);
            LOG_INFO("HTTP Range request: %s", range.c_str());
        }

        int code = http_.GET();

        // 200 OK o 206 Partial Content
        if (code != 200 && code != 206) {
            LOG_ERROR("HTTP GET failed: %d %s", code, http_.errorToString(code).c_str());
            http_.end();
            return false;
        }

        stream_ = http_.getStreamPtr();
        if (stream_) {
            stream_->setTimeout(3000);  // blocco massimo per readBytes
        }
        position_ = from_position;
        buffer_fill_ = 0;
        buffer_pos_ = 0;

        LOG_INFO("HTTP connected, code=%d, position=%u", code, (unsigned)position_);
        return true;
    }

    HTTPClient http_;
    WiFiClient* stream_ = nullptr;
    String url_;
    size_t content_length_ = 0;
    size_t position_ = 0;
    bool supports_range_ = false;

    uint8_t local_buffer_[4096];  // Buffer per chunk HTTP
    size_t buffer_fill_ = 0;
    size_t buffer_pos_ = 0;
};
