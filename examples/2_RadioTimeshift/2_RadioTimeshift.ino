/*
 * Radio Streaming with Timeshift Example
 *
 * This example demonstrates how to stream internet radio with timeshift capabilities
 * (pause, rewind, and seek) using the openESPaudio library.
 *
 * The timeshift feature allows you to:
 * - Pause live radio and resume later
 * - Seek backward to replay content you missed
 * - Buffer audio in PSRAM or SD card
 *
 * Hardware requirements:
 * - ESP32-S3 board with I2S DAC
 * - WiFi connection
 * - PSRAM recommended for best performance
 * - Optional: SD card for extended buffer
 *
 * Copyright (c) 2025 rederyk
 * Licensed under the MIT License
 */

#include <Arduino.h>
#include <WiFi.h>
#include <openESPaudio.h>

// ===== CONFIGURATION - MODIFY THESE =====
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* RADIO_URL = "http://stream.radioparadise.com/mp3-128";

// Storage mode for timeshift buffer:
// - StorageMode::PSRAM_ONLY: Fast, ~2min buffer (requires PSRAM)
// - StorageMode::SD_CARD: Slower, unlimited buffer (requires SD card)
StorageMode TIMESHIFT_STORAGE = StorageMode::PSRAM_ONLY;
// ========================================

AudioPlayer player;

void connectWiFi() {
  Serial.printf("Connecting to WiFi: %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int timeout = 20;
  while (WiFi.status() != WL_CONNECTED && timeout > 0) {
    delay(500);
    Serial.print(".");
    timeout--;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nERROR: WiFi connection failed!");
    Serial.println("Check your credentials and try again.");
  }
}

void startRadioStream() {
  // Create timeshift manager
  auto* ts = new TimeshiftManager();

  // Configure storage mode BEFORE opening
  ts->setStorageMode(TIMESHIFT_STORAGE);
  Serial.printf("Timeshift mode: %s\n",
                TIMESHIFT_STORAGE == StorageMode::PSRAM_ONLY ? "PSRAM" : "SD Card");

  // Open the stream URL
  Serial.printf("Opening stream: %s\n", RADIO_URL);
  if (!ts->open(RADIO_URL)) {
    Serial.println("ERROR: Failed to open stream URL!");
    Serial.println("Check your WiFi connection and URL.");
    delete ts;
    return;
  }

  // Start download task
  if (!ts->start()) {
    Serial.println("ERROR: Failed to start download task!");
    delete ts;
    return;
  }

  Serial.println("Downloading... waiting for first chunk");

  // Wait for first chunk to be ready (max 10 seconds)
  uint32_t start_wait = millis();
  while (ts->buffered_bytes() == 0) {
    if (millis() - start_wait > 10000) {
      Serial.println("ERROR: Timeout waiting for stream data!");
      delete ts;
      return;
    }
    delay(100);
  }

  Serial.println("First chunk ready! Starting playback...");

  // Set up auto-pause callback for buffering
  ts->set_auto_pause_callback([](bool should_pause) {
    player.set_pause(should_pause);
    if (should_pause) {
      Serial.println("Auto-pause: Buffering...");
    } else {
      Serial.println("Auto-pause: Resuming playback");
    }
  });

  // Transfer ownership to player
  player.select_source(std::unique_ptr<IDataSource>(ts));

  // Load and start playback
  if (!player.arm_source()) {
    Serial.println("ERROR: Failed to arm source!");
    return;
  }

  player.start();
  Serial.println("\n=== Radio streaming started! ===");
  Serial.println("Commands:");
  Serial.println("  p - Pause/Resume");
  Serial.println("  q - Stop");
  Serial.println("  [ - Seek back 5 seconds");
  Serial.println("  ] - Seek forward 5 seconds");
  Serial.println("  v50 - Set volume to 50%");
  Serial.println("  i - Show status");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== openESPaudio - Radio Timeshift Example ===");

  // Connect to WiFi (REQUIRED for streaming)
  connectWiFi();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot start streaming without WiFi!");
    return;
  }

  // Optional: Initialize SD card if using SD_CARD mode
  if (TIMESHIFT_STORAGE == StorageMode::SD_CARD) {
    Serial.println("Initializing SD card...");
    if (SdCardDriver::getInstance().begin()) {
      Serial.println("SD card mounted successfully");
    } else {
      Serial.println("WARNING: SD card mount failed!");
      Serial.println("Error: " + SdCardDriver::getInstance().lastError());
      Serial.println("Switching to PSRAM mode...");
      TIMESHIFT_STORAGE = StorageMode::PSRAM_ONLY;
    }
  }

  // Start radio stream
  startRadioStream();
}

void loop() {
  // REQUIRED: Process audio housekeeping
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
    } else if (cmd == "[") {
      // Seek back 5 seconds
      uint32_t current = player.current_position_sec();
      int target = (int)current - 5;
      if (target < 0) target = 0;
      Serial.printf("Seeking back to %d sec\n", target);
      player.request_seek(target);
    } else if (cmd == "]") {
      // Seek forward 5 seconds
      uint32_t current = player.current_position_sec();
      uint32_t total = player.total_duration_sec();
      uint32_t target = current + 5;
      if (total > 0 && target > total) target = total;
      Serial.printf("Seeking forward to %u sec\n", target);
      player.request_seek(target);
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

    if (player.state() == PlayerState::PLAYING || player.state() == PlayerState::PAUSED) {
      uint32_t pos_sec = player.current_position_sec();
      uint32_t total_sec = player.total_duration_sec();
      Serial.printf("Position: %02u:%02u / %02u:%02u (buffered)\n",
                    pos_sec / 60, pos_sec % 60,
                    total_sec / 60, total_sec % 60);
    }
  }

  delay(10);
}
