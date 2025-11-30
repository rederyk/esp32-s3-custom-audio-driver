/*
 * Advanced Control Example
 *
 * This example demonstrates advanced features including:
 * - Serial command interface
 * - File browsing (LittleFS and SD card)
 * - Dynamic source selection
 * - Volume control
 * - Seek operations
 * - Status monitoring
 *
 * This is similar to the original main.cpp application, but restructured
 * as a library example with user-controlled WiFi and filesystem initialization.
 *
 * Copyright (c) 2025 rederyk
 * Licensed under the MIT License
 */

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <openESPaudio.h>

// ===== USER CONFIGURATION =====
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* DEFAULT_FILE = "/sample.mp3";
const char* DEFAULT_RADIO_URL = "http://stream.radioparadise.com/mp3-128";
// ==============================

AudioPlayer player;
StorageMode preferred_storage_mode = StorageMode::PSRAM_ONLY;

void listLittleFSFiles(const char* path) {
  File root = LittleFS.open(path);
  if (!root || !root.isDirectory()) {
    Serial.printf("ERROR: Cannot open directory %s\n", path);
    return;
  }

  Serial.printf("=== LittleFS: %s ===\n", path);
  File entry = root.openNextFile();
  if (!entry) {
    Serial.println("(empty)");
  }
  while (entry) {
    if (entry.isDirectory()) {
      Serial.printf("DIR  %s\n", entry.name());
    } else {
      Serial.printf("FILE %s (%u bytes)\n", entry.name(), entry.size());
    }
    entry = root.openNextFile();
  }
  root.close();
}

void listSDFiles(const char* path) {
  auto& sd = SdCardDriver::getInstance();
  if (!sd.isMounted()) {
    Serial.println("SD card not mounted");
    return;
  }

  Serial.printf("=== SD Card: %s ===\n", path);
  auto entries = sd.listDirectory(path);
  if (entries.empty()) {
    Serial.println("(empty)");
  }
  for (const auto& entry : entries) {
    Serial.printf("%s %s (%llu bytes)\n",
                  entry.isDirectory ? "DIR " : "FILE",
                  entry.name.c_str(),
                  entry.sizeBytes);
  }
}

void startTimeshiftRadio(const char* url) {
  // Stop current playback
  if (player.state() == PlayerState::PLAYING || player.state() == PlayerState::PAUSED) {
    Serial.println("Stopping current playback...");
    player.stop();
    delay(500);
  }

  // Create and configure timeshift manager
  auto* ts = new TimeshiftManager();
  ts->setStorageMode(preferred_storage_mode);

  Serial.printf("Opening stream: %s\n", url);
  Serial.printf("Storage mode: %s\n",
                preferred_storage_mode == StorageMode::PSRAM_ONLY ? "PSRAM" : "SD Card");

  if (!ts->open(url)) {
    Serial.println("ERROR: Failed to open stream!");
    delete ts;
    return;
  }

  if (!ts->start()) {
    Serial.println("ERROR: Failed to start download!");
    delete ts;
    return;
  }

  Serial.println("Waiting for first chunk...");
  uint32_t start_wait = millis();
  while (ts->buffered_bytes() == 0) {
    if (millis() - start_wait > 10000) {
      Serial.println("ERROR: Timeout!");
      delete ts;
      return;
    }
    delay(100);
  }

  Serial.println("First chunk ready!");

  // Set auto-pause callback
  ts->set_auto_pause_callback([](bool should_pause) {
    player.set_pause(should_pause);
  });

  // Transfer to player
  player.select_source(std::unique_ptr<IDataSource>(ts));
  if (!player.arm_source()) {
    Serial.println("ERROR: Failed to arm source!");
    return;
  }

  player.start();
  Serial.println("Streaming started!");
}

void showHelp() {
  Serial.println("\n=== AVAILABLE COMMANDS ===");
  Serial.println("PLAYBACK:");
  Serial.println("  r - Start radio streaming with timeshift");
  Serial.println("  u<url> - Stream from custom URL (e.g., uhttp://example.com/stream)");
  Serial.println("  f<path> - Play file (e.g., f/sample.mp3)");
  Serial.println("  p - Play/Pause toggle");
  Serial.println("  q - Stop playback");
  Serial.println();
  Serial.println("CONTROL:");
  Serial.println("  v## - Set volume (e.g., v75 = 75%)");
  Serial.println("  [ - Seek back 5 seconds");
  Serial.println("  ] - Seek forward 5 seconds");
  Serial.println("  s## - Seek to position (e.g., s30 = 30 seconds)");
  Serial.println("  i - Show player status");
  Serial.println();
  Serial.println("FILESYSTEM:");
  Serial.println("  d [path] - List files (e.g., 'd /' or 'd /sd/')");
  Serial.println("  x - SD card status");
  Serial.println();
  Serial.println("TIMESHIFT STORAGE:");
  Serial.println("  W - Show preferred storage mode");
  Serial.println("  Z - Use PSRAM mode (fast, ~2min buffer)");
  Serial.println("  C - Use SD Card mode (slower, unlimited buffer)");
  Serial.println();
  Serial.println("DEBUG:");
  Serial.println("  m - Memory stats");
  Serial.println("  h - Show this help");
  Serial.println();
}

void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  char first = cmd.charAt(0);

  if (cmd.length() == 1) {
    switch (first) {
      case 'h':
      case 'H':
        showHelp();
        break;

      case 'r':
      case 'R':
        startTimeshiftRadio(DEFAULT_RADIO_URL);
        break;

      case 'p':
      case 'P':
        player.toggle_pause();
        Serial.println(player.state() == PlayerState::PAUSED ? "PAUSED" : "PLAYING");
        break;

      case 'q':
      case 'Q':
        player.stop();
        Serial.println("STOPPED");
        break;

      case 'd':
      case 'D':
        listLittleFSFiles("/");
        break;

      case 'i':
      case 'I':
        player.print_status();
        break;

      case 'm':
      case 'M':
        Serial.printf("Heap free: %u bytes\n", esp_get_free_heap_size());
        break;

      case 'x':
      case 'X': {
        auto& sd = SdCardDriver::getInstance();
        Serial.println("=== SD Card Status ===");
        if (sd.isMounted()) {
          Serial.printf("Mounted: %s\n", sd.cardTypeString().c_str());
          Serial.printf("Size: %llu MB, Used: %llu MB\n",
                       sd.totalBytes() / (1024 * 1024),
                       sd.usedBytes() / (1024 * 1024));
        } else {
          Serial.printf("Not mounted: %s\n", sd.lastError().c_str());
        }
        break;
      }

      case 'w':
      case 'W':
        Serial.printf("Timeshift storage mode: %s\n",
                     preferred_storage_mode == StorageMode::PSRAM_ONLY ? "PSRAM" : "SD Card");
        break;

      case 'z':
      case 'Z':
        preferred_storage_mode = StorageMode::PSRAM_ONLY;
        Serial.println("Storage mode set to: PSRAM");
        break;

      case 'c':
      case 'C':
        preferred_storage_mode = StorageMode::SD_CARD;
        Serial.println("Storage mode set to: SD Card");
        break;

      case '[': {
        uint32_t current = player.current_position_sec();
        int target = (int)current - 5;
        if (target < 0) target = 0;
        Serial.printf("Seeking to %d sec\n", target);
        player.request_seek(target);
        break;
      }

      case ']': {
        uint32_t current = player.current_position_sec();
        uint32_t total = player.total_duration_sec();
        uint32_t target = current + 5;
        if (total > 0 && target > total) target = total;
        Serial.printf("Seeking to %u sec\n", target);
        player.request_seek(target);
        break;
      }

      default:
        Serial.println("Unknown command. Type 'h' for help.");
        break;
    }
  } else {
    // Multi-character commands
    if (first == 'v' || first == 'V') {
      int volume = cmd.substring(1).toInt();
      if (volume >= 0 && volume <= 100) {
        player.set_volume(volume);
        Serial.printf("Volume: %d%%\n", volume);
      }
    } else if (first == 's' || first == 'S') {
      int target = cmd.substring(1).toInt();
      if (target >= 0) {
        Serial.printf("Seeking to %d sec\n", target);
        player.request_seek(target);
      }
    } else if (first == 'd' || first == 'D') {
      String path = cmd.substring(1);
      path.trim();
      if (path.startsWith("/sd")) {
        listSDFiles(path.c_str());
      } else {
        if (!path.startsWith("/")) path = "/" + path;
        listLittleFSFiles(path.c_str());
      }
    } else if (first == 'f' || first == 'F') {
      String path = cmd.substring(1);
      path.trim();
      if (!path.startsWith("/") && !path.startsWith("http")) {
        path = "/" + path;
      }
      Serial.printf("Selecting file: %s\n", path.c_str());
      player.select_source(path.c_str());
      player.arm_source();
      player.start();
    } else if (first == 'u' || first == 'U') {
      String url = cmd.substring(1);
      url.trim();
      startTimeshiftRadio(url.c_str());
    } else {
      Serial.println("Unknown command. Type 'h' for help.");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== openESPaudio - Advanced Control Example ===");
  Serial.println("Type 'h' for help\n");

  // Initialize LittleFS
  if (!LittleFS.begin()) {
    Serial.println("ERROR: LittleFS mount failed!");
  } else {
    Serial.println("LittleFS mounted");
  }

  // Initialize SD card
  if (SdCardDriver::getInstance().begin()) {
    Serial.println("SD card mounted");
  } else {
    Serial.println("SD card not available");
  }

  // Connect to WiFi
  if (strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0) {
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
      Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println("\nWiFi connection failed");
    }
  } else {
    Serial.println("WiFi not configured (update WIFI_SSID/WIFI_PASSWORD)");
  }

  Serial.println("\nReady! Type 'h' for help.");
}

void loop() {
  // REQUIRED: Process audio
  player.tick_housekeeping();

  // Handle commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    handleCommand(cmd);
  }

  // Status updates
  static uint32_t last_log = 0;
  if (millis() - last_log > 10000) {
    last_log = millis();

    if (player.state() == PlayerState::PLAYING) {
      uint32_t pos = player.current_position_sec();
      uint32_t total = player.total_duration_sec();
      Serial.printf("Playing: %02u:%02u / %02u:%02u\n",
                    pos / 60, pos % 60,
                    total / 60, total % 60);
    }
  }

  delay(10);
}
