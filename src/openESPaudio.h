// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.

#pragma once

/**
 * @file openESPaudio.h
 * @brief Main header file for the openESPaudio library
 *
 * This library provides audio playback capabilities for ESP32 boards with support for:
 * - Local file playback from LittleFS and SD card
 * - HTTP streaming with timeshift buffer
 * - Multiple audio formats (MP3, WAV)
 * - Seek support for local files and buffered streams
 * - Volume control and playback management
 *
 * @author rederyk
 * @version 1.0.0
 */

// Core player functionality
#include "audio_player.h"
#include "audio_types.h"

// Timeshift manager for streaming
#include "timeshift_manager.h"

// Data sources
#include "data_source.h"
#include "data_source_littlefs.h"
#include "data_source_sdcard.h"
#include "data_source_http.h"

// Drivers (optional - user may need for manual initialization)
#include "drivers/sd_card_driver.h"

// Logger utility
#include "logger.h"

/**
 * @brief openESPaudio library namespace (currently using global scope for Arduino compatibility)
 *
 * Main classes:
 * - AudioPlayer: Main audio playback controller
 * - TimeshiftManager: Streaming source with timeshift capabilities
 * - SdCardDriver: SD card access singleton
 *
 * Usage example:
 * @code
 * #include <openESPaudio.h>
 *
 * AudioPlayer player;
 *
 * void setup() {
 *   Serial.begin(115200);
 *   WiFi.begin("SSID", "password");
 *
 *   player.select_source("http://stream.example.com/radio.mp3");
 *   player.arm_source();
 *   player.start();
 * }
 *
 * void loop() {
 *   player.tick_housekeeping();
 *   delay(10);
 * }
 * @endcode
 */
