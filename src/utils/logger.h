// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#pragma once

#include "../logger.h"

// Wrapper singleton per compatibilità con SdCardDriver
class Logger {
public:
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    void error(const char* msg) {
        LOG_ERROR("%s", msg);
    }

    void warn(const char* msg) {
        LOG_WARN("%s", msg);
    }

    void info(const char* msg) {
        LOG_INFO("%s", msg);
    }

    void infof(const char* format, ...) {
        va_list args;
        va_start(args, format);
        char buffer[256];
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        LOG_INFO("%s", buffer);
    }

private:
    Logger() = default;
};
