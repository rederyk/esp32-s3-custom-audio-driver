// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.


#ifndef LOG_H
#define LOG_H

#include <Arduino.h>

// Livelli di logging
enum class LogLevel {
    ERROR = 0,
    WARN = 1,
    INFO = 2,
    DEBUG = 3
};

// Funzione principale per loggare
void log_message(LogLevel level, const char* format, ...);

// Funzioni di convenienza
#define LOG_ERROR(format, ...) log_message(LogLevel::ERROR, "[ERROR] " format "\n", ##__VA_ARGS__)
#define LOG_WARN(format, ...)  log_message(LogLevel::WARN,  "[WARN]  " format "\n", ##__VA_ARGS__)
#define LOG_INFO(format, ...)  log_message(LogLevel::INFO,  "[INFO]  " format "\n", ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) log_message(LogLevel::DEBUG, "[DEBUG] " format "\n", ##__VA_ARGS__)

// Imposta livello minimo di logging (solo messaggi >= livello verranno mostrati)
void set_log_level(LogLevel level);
LogLevel get_log_level();

#endif // LOG_H
