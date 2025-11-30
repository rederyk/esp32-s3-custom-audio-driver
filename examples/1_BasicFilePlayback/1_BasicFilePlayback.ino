/*
 * Basic File Playback Example
 *
 * This example demonstrates how to play an audio file from LittleFS or SD card
 * using the openESPaudio library.
 *
 * Hardware requirements:
 * - ESP32-S3 board with I2S DAC (e.g., ESP32-S3-DevKitM with ES8311 codec)
 * - Optional: SD card for additional storage
 *
 * Setup:
 * 1. Upload an MP3 or WAV file to LittleFS using PlatformIO: pio run -t uploadfs
 * 2. Update the FILE_PATH below to match your file
 * 3. Upload and run this sketch
 *
 * Copyright (c) 2025 rederyk
 * Licensed under the MIT License
 */

#include <Arduino.h>
#include <LittleFS.h>
#include <openESPaudio.h>

// Configuration
const char* FILE_PATH = "/sample.mp3";  // Change this to your audio file path

// Create audio player instance
AudioPlayer player;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== openESPaudio - Basic File Playback ===");

  // Initialize LittleFS
  if (!LittleFS.begin()) {
    Serial.println("ERROR: LittleFS mount failed!");
    Serial.println("Upload filesystem with: pio run -t uploadfs");
    return;
  }
  Serial.println("LittleFS mounted successfully");

  // Optional: Initialize SD card if you want to play from SD
  // if (SdCardDriver::getInstance().begin()) {
  //   Serial.println("SD card mounted successfully");
  // }

  // Select audio source
  Serial.printf("Selecting file: %s\n", FILE_PATH);
  if (!player.select_source(FILE_PATH)) {
    Serial.println("ERROR: Failed to select source!");
    return;
  }

  // Load the file (arm the decoder)
  Serial.println("Loading file...");
  if (!player.arm_source()) {
    Serial.println("ERROR: Failed to load file!");
    return;
  }

  // Start playback
  Serial.println("Starting playback...");
  player.start();

  Serial.println("\nPlayback started!");
  Serial.println("Commands:");
  Serial.println("  p - Pause/Resume");
  Serial.println("  q - Stop");
  Serial.println("  v50 - Set volume to 50%");
  Serial.println("  i - Show status");
}

void loop() {
  // Process audio housekeeping (REQUIRED - call this regularly!)
  player.tick_housekeeping();

  // Handle serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.length() == 0) {
      // Ignore empty commands
    } else if (cmd == "p" || cmd == "P") {
      player.toggle_pause();
      Serial.println(player.state() == PlayerState::PAUSED ? "PAUSED" : "PLAYING");
    } else if (cmd == "q" || cmd == "Q") {
      player.stop();
      Serial.println("STOPPED");
    } else if (cmd == "i" || cmd == "I") {
      player.print_status();
    } else if (cmd.startsWith("v") || cmd.startsWith("V")) {
      int volume = cmd.substring(1).toInt();
      if (volume >= 0 && volume <= 100) {
        player.set_volume(volume);
        Serial.printf("Volume set to %d%%\n", volume);
      } else {
        Serial.println("Invalid volume (use 0-100)");
      }
    } else {
      Serial.println("Unknown command");
    }
  }

  // Print status every 5 seconds
  static uint32_t last_status = 0;
  if (millis() - last_status > 5000) {
    last_status = millis();

    if (player.state() == PlayerState::PLAYING) {
      uint32_t pos_sec = player.current_position_sec();
      uint32_t total_sec = player.total_duration_sec();
      Serial.printf("Playing: %02u:%02u / %02u:%02u\n",
                    pos_sec / 60, pos_sec % 60,
                    total_sec / 60, total_sec % 60);
    } else if (player.state() == PlayerState::ENDED) {
      Serial.println("Playback ended");
    }
  }

  delay(10);
}
