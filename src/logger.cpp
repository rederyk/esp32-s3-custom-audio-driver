// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#include "logger.h"
#include <cstdarg>
#include <cstdio>

namespace openespaudio {

// Livello di logging corrente
static LogLevel current_level = LogLevel::INFO;

void log_message(LogLevel level, const char* format, ...) {
    if (level > current_level) {
        return;  // Non loggare messaggi sotto il livello corrente
    }

    va_list args;
    va_start(args, format);
    char buffer[256];  // Buffer per la stringa formattata
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    Serial.print(buffer);  // Output senza newline, poiché includiamo \n nel format
}

void set_log_level(LogLevel level) {
    current_level = level;
}

LogLevel get_log_level() {
    return current_level;
}

} // namespace openespaudio
