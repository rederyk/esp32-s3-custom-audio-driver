# openESPaudio

A comprehensive audio playback library for ESP32 boards with support for local files, HTTP streaming, and timeshift capabilities.

## Features

- **Multiple Audio Sources**
  - Local file playback from LittleFS and SD card
  - HTTP streaming from internet radio and other sources
  - Automatic source type detection

- **Audio Format Support**
  - MP3 (via dr_mp3/minimp3)
  - WAV (PCM)
  - Extensible decoder architecture

- **Timeshift Capabilities**
  - Pause and rewind live radio streams
  - Buffer in PSRAM (fast, ~2min) or SD card (unlimited)
  - Seek support for buffered content
  - Auto-pause when buffering needed

- **Playback Control**
  - Play, pause, stop, resume
  - Volume control (0-100%)
  - Seek by time (seconds/milliseconds)
  - Playback status and progress tracking

- **Hardware Support**
  - Optimized for ESP32-S3 with PSRAM
  - I2S audio output (ES8311 codec and compatible)
  - SD card support via SD_MMC or SPI
  - LittleFS for onboard storage

## Hardware Requirements

### Minimum Requirements
- ESP32 or ESP32-S3 board
- I2S DAC (e.g., ES8311, PCM5102, MAX98357A)
- For streaming: WiFi connection

### Recommended
- ESP32-S3 with 2MB+ PSRAM (for timeshift and better performance)
- SD card for extended storage and buffering
- 16MB flash for LittleFS storage

### Tested Hardware
- Freenove ESP32-S3-WROOM with ES8311 codec
- ESP32-S3-DevKitM-1 with external DAC

## Installation

### PlatformIO (Recommended)

1. Add the library to your `platformio.ini`:

```ini
[env:your_board]
platform = espressif32
board = esp32-s3-devkitm-1
framework = arduino
lib_deps =
    https://github.com/yourusername/openESPaudio.git
```

2. Or clone this repository into your project's `lib` folder:

```bash
cd your_project/lib
git clone https://github.com/yourusername/openESPaudio.git
```

### Arduino IDE

1. Download this repository as ZIP
2. In Arduino IDE: Sketch → Include Library → Add .ZIP Library
3. Select the downloaded ZIP file

## Configuration

### PlatformIO Build Flags

For optimal performance with ESP32-S3 and PSRAM, add these build flags to your `platformio.ini`:

```ini
[env:esp32-s3-board]
platform = espressif32
board = esp32-s3-devkitm-1
framework = arduino
build_flags =
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DBOARD_HAS_PSRAM
    -mfix-esp32-psram-cache-issue
board_build.filesystem = littlefs
board_build.partitions = default_16MB.csv
board_build.arduino.memory_type = qio_opi
board_build.psram.enable = true
board_build.psram.mode = opi
board_build.psram.freq = 80
```

## Quick Start

### Basic File Playback

```cpp
#include <Arduino.h>
#include <LittleFS.h>
#include <openESPaudio.h>

AudioPlayer player;

void setup() {
  Serial.begin(115200);

  // Initialize filesystem
  LittleFS.begin();

  // Select and play a file
  player.select_source("/music/song.mp3");
  player.arm_source();
  player.start();
}

void loop() {
  player.tick_housekeeping();  // REQUIRED - call regularly!
  delay(10);
}
```

### Radio Streaming with Timeshift

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <openESPaudio.h>

AudioPlayer player;

void setup() {
  Serial.begin(115200);

  // Connect to WiFi
  WiFi.begin("YOUR_SSID", "YOUR_PASSWORD");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  // Create timeshift manager
  auto* ts = new TimeshiftManager();
  ts->setStorageMode(StorageMode::PSRAM_ONLY);  // or SD_CARD

  // Open stream
  ts->open("http://stream.example.com/radio.mp3");
  ts->start();

  // Wait for first chunk
  while (ts->buffered_bytes() == 0) {
    delay(100);
  }

  // Transfer to player and start
  player.select_source(std::unique_ptr<IDataSource>(ts));
  player.arm_source();
  player.start();
}

void loop() {
  player.tick_housekeeping();
  delay(10);
}
```

## Examples

The library includes three comprehensive examples:

1. **[1_BasicFilePlayback](examples/1_BasicFilePlayback/)** - Simple file playback from LittleFS
2. **[2_RadioTimeshift](examples/2_RadioTimeshift/)** - Internet radio streaming with timeshift
3. **[3_AdvancedControl](examples/3_AdvancedControl/)** - Full-featured application with serial commands

See the `examples/` folder for complete, working code.

## API Reference

### AudioPlayer

Main class for audio playback control.

#### Methods

```cpp
// Source selection
bool select_source(const char* uri, SourceType hint = SourceType::LITTLEFS);
bool select_source(std::unique_ptr<IDataSource> source);

// Playback control
bool arm_source();          // Load and prepare source
void start();               // Start playback
void stop();                // Stop playback
void toggle_pause();        // Toggle pause state
void set_pause(bool pause); // Set pause state directly

// Volume and seeking
void set_volume(int vol_pct);      // 0-100%
void request_seek(int seconds);     // Seek to position

// Status and monitoring
PlayerState state() const;
bool is_playing() const;
uint32_t current_position_sec() const;
uint32_t current_position_ms() const;
uint32_t total_duration_sec() const;
uint32_t total_duration_ms() const;
void print_status() const;

// Housekeeping (REQUIRED)
void tick_housekeeping();  // Call this regularly in loop()
```

#### Player States

```cpp
enum class PlayerState {
    STOPPED,  // No playback
    PLAYING,  // Currently playing
    PAUSED,   // Playback paused
    ENDED,    // Playback completed
    ERROR     // Error occurred
};
```

### TimeshiftManager

Manages HTTP streaming with timeshift buffer.

#### Methods

```cpp
// Configuration
void setStorageMode(StorageMode mode);  // PSRAM_ONLY or SD_CARD
StorageMode getStorageMode() const;

// Control
bool open(const char* uri);    // Open stream URL
bool start();                  // Start download task
void stop();                   // Stop download
void pause_recording();        // Pause buffer recording
void resume_recording();       // Resume buffer recording

// Status
size_t buffered_bytes() const;
size_t total_downloaded_bytes() const;
uint32_t total_duration_ms() const;

// Auto-pause configuration
void set_auto_pause_callback(std::function<void(bool)> callback);
void set_auto_pause_margin(uint32_t delay_ms, size_t min_chunks);
```

#### Storage Modes

```cpp
enum class StorageMode {
    SD_CARD,    // Slower, unlimited buffer, requires SD card
    PSRAM_ONLY  // Fast, ~2min buffer, requires PSRAM
};
```

### SdCardDriver

Singleton for SD card access.

```cpp
// Get instance
auto& sd = SdCardDriver::getInstance();

// Methods
bool begin();                           // Initialize SD card
bool isMounted() const;                 // Check if mounted
String lastError() const;               // Get last error
uint64_t totalBytes() const;            // Total card size
uint64_t usedBytes() const;             // Used space
String cardTypeString() const;          // Card type name
vector<DirectoryEntry> listDirectory(const char* path);
```

## WiFi Management

**IMPORTANT**: The library does NOT manage WiFi connections. Your application is responsible for:
- Connecting to WiFi before using HTTP streaming
- Maintaining the WiFi connection
- Handling reconnection if needed

The library will check if WiFi is connected when opening HTTP streams and fail gracefully if not.

Example:

```cpp
void setup() {
  // YOUR responsibility
  WiFi.begin("SSID", "PASSWORD");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  // Now you can use HTTP streaming
  player.select_source("http://...");
}
```

This design allows maximum flexibility - you can use WiFiManager, SmartConfig, or any other connection method you prefer.

## Filesystem Management

Similarly, you must initialize filesystems before use:

```cpp
void setup() {
  // Initialize LittleFS if playing from internal storage
  LittleFS.begin();

  // Initialize SD card if using SD storage
  SdCardDriver::getInstance().begin();

  // Now you can use the library
  player.select_source("/littlefs/file.mp3");
  // or
  player.select_source("/sd/file.mp3");
}
```

## Troubleshooting

### No audio output
- Check I2S pin configuration matches your hardware
- Verify DAC is properly initialized
- Check volume level (`set_volume(75)`)
- Test with known-good audio file

### WiFi streaming fails
- Ensure WiFi is connected BEFORE calling `select_source()`
- Check URL is accessible (test in browser)
- Verify firewall/proxy settings
- Check memory availability (use PSRAM if available)

### Seek not working
- Local files: Ensure file format supports seeking (MP3 does, some streams don't)
- Timeshift: Ensure buffer has accumulated enough data
- Check if source reports as seekable: `data_source()->is_seekable()`

### Out of memory
- Enable PSRAM in your board configuration
- Use SD card storage mode for timeshift
- Reduce buffer sizes if needed
- Check for memory leaks with `esp_get_free_heap_size()`

### SD card not mounting
- Verify SD card is formatted as FAT32
- Check SD card pins in `SdCardDriver` configuration
- Try a different SD card
- Enable debug logging: `set_log_level(LogLevel::DEBUG)`

## Performance Tips

1. **Use PSRAM** for timeshift buffering when available
2. **Call `tick_housekeeping()` regularly** - critical for proper operation
3. **Use appropriate buffer modes** - PSRAM for speed, SD for capacity
4. **Monitor memory** - check heap regularly with debug builds
5. **Keep WiFi stable** - implement reconnection logic for streaming

## Logging

The library includes a built-in logger. Configure log level:

```cpp
#include <logger.h>

void setup() {
  set_log_level(LogLevel::DEBUG);  // DEBUG, INFO, WARN, ERROR
}
```

## License

MIT License - see [LICENSE](LICENSE) file for details.

Copyright (c) 2025 rederyk

## Credits

This library uses:
- [dr_mp3/minimp3](https://github.com/mackron/dr_libs) - MP3 decoder (Public Domain/MIT-0)
- ESP-IDF components - WiFi, SD, HTTP (Apache 2.0)
- Arduino framework (LGPL 2.1+)

## Contributing

Contributions are welcome! Please:
1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Submit a pull request

## Support

- **Issues**: [GitHub Issues](https://github.com/yourusername/openESPaudio/issues)
- **Discussions**: [GitHub Discussions](https://github.com/yourusername/openESPaudio/discussions)
- **Documentation**: See examples and API reference above

## Changelog

### Version 1.0.0 (2025-01-XX)
- Initial release
- MP3 and WAV support
- LittleFS and SD card sources
- HTTP streaming with timeshift
- Seek support
- Volume control
- Auto-pause buffering
