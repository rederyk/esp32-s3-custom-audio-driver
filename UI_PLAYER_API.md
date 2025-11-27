# Audio Player UI API Documentation

Complete API reference for building a Music Player UI with progress bar, controls, and metadata display.

## Table of Contents
- [Callbacks](#callbacks)
- [Playback Control Methods](#playback-control-methods)
- [UI Information Methods](#ui-information-methods)
- [Progress Bar Integration](#progress-bar-integration)
- [Example Usage](#example-usage)

---

## Callbacks

Register callbacks to receive player events:

```cpp
PlayerCallbacks callbacks;
callbacks.on_start = my_start_handler;
callbacks.on_stop = my_stop_handler;
callbacks.on_end = my_end_handler;
callbacks.on_error = my_error_handler;
callbacks.on_metadata = my_metadata_handler;
callbacks.on_progress = my_progress_handler;  // NEW! Auto-updates every 250ms

player.set_callbacks(callbacks);
```

### Callback Signatures

```cpp
void on_start(const char* path);
void on_stop(const char* path, PlayerState state);
void on_end(const char* path);
void on_error(const char* path, const char* detail);
void on_metadata(const Metadata& meta, const char* path);
void on_progress(uint32_t position_ms, uint32_t duration_ms);  // NEW!
```

### Callback Details

#### `on_start(path)`
Called when playback starts.
- **When**: After `player.start()` successfully initializes
- **Use**: Update UI to show "Playing" state, enable pause/stop buttons

#### `on_stop(path, state)`
Called when playback stops.
- **When**: After `player.stop()` or auto-recovery
- **Parameters**:
  - `state`: Final PlayerState (STOPPED, ERROR, etc.)
- **Use**: Update UI to show "Stopped", reset progress bar

#### `on_end(path)`
Called when track finishes naturally.
- **When**: Audio stream reaches end
- **Use**: Auto-play next track, show "Ended" state

#### `on_error(path, detail)`
Called on errors.
- **Parameters**:
  - `detail`: Human-readable error description
- **Use**: Show error dialog/toast to user

#### `on_metadata(meta, path)`
Called when metadata is parsed (ID3 tags for MP3, etc.).
- **When**: After `arm_source()` for seekable sources
- **Parameters**:
  - `meta`: Metadata structure with title, artist, album, genre, year, track, comment, cover_present
- **Use**: Update "Now Playing" display

#### `on_progress(position_ms, duration_ms)` **NEW!**
Called automatically every **250ms** during playback.
- **Parameters**:
  - `position_ms`: Current playback position in milliseconds
  - `duration_ms`: Total track duration in milliseconds
- **Use**: Update progress bar, time labels
- **Note**: Only called during active playback (not when paused/stopped)

---

## Playback Control Methods

### Load and Start

```cpp
// 1. Select audio source
bool select_source(const char* uri, SourceType hint = SourceType::LITTLEFS);
bool select_source(std::unique_ptr<IDataSource> source);

// 2. Arm (load metadata, validate)
bool arm_source();

// 3. Start playback
void start();
```

**Example**:
```cpp
if (player.select_source("/music/song.mp3")) {
    if (player.arm_source()) {
        player.start();
    }
}
```

### Playback Control

```cpp
void stop();                    // Stop playback completely
void toggle_pause();            // Toggle between play/pause
void set_pause(bool pause);     // Programmatically set pause state
void request_seek(int seconds); // Seek to position (in seconds)
void set_volume(int vol_pct);   // Set volume (0-100%)
```

**Examples**:
```cpp
// Pause/Resume toggle
player.toggle_pause();

// Programmatic pause
player.set_pause(true);   // Pause
player.set_pause(false);  // Resume

// Seek to 30 seconds
player.request_seek(30);

// Set volume to 75%
player.set_volume(75);
```

---

## UI Information Methods

### Playback State

```cpp
PlayerState state() const;      // Current state (STOPPED, PLAYING, PAUSED, ENDED, ERROR)
bool is_playing() const;        // Quick check if playing
```

**PlayerState enum**:
- `PlayerState::STOPPED` - Not playing
- `PlayerState::PLAYING` - Currently playing
- `PlayerState::PAUSED` - Paused
- `PlayerState::ENDED` - Track finished
- `PlayerState::ERROR` - Error occurred

### Position & Duration

```cpp
// Time in milliseconds
uint32_t current_position_ms() const;
uint32_t total_duration_ms() const;

// Time in seconds
uint32_t current_position_sec() const;
uint32_t total_duration_sec() const;

// Frame-level (for internal use)
uint64_t played_frames() const;
uint64_t total_frames() const;
uint32_t current_sample_rate() const;
```

### Audio Format Info

```cpp
uint32_t current_bitrate() const;      // NEW! Bitrate in kbps
const char* current_uri() const;       // Current file/stream URI
SourceType source_type() const;        // LITTLEFS, SD_CARD, HTTP_STREAM
AudioFormat format() const;            // MP3, WAV, AAC, FLAC
```

**AudioFormat** is accessible via:
```cpp
player.stream_->format()  // Returns AudioFormat enum
```

### Volume

```cpp
int current_volume() const;  // Current volume (0-100%)
int saved_volume() const;    // Saved volume before pause
int user_volume() const;     // User-set volume
```

### Metadata

```cpp
const Metadata& metadata() const;
```

**Metadata structure**:
```cpp
struct Metadata {
    String title;
    String artist;
    String album;
    String genre;
    String year;
    String track;
    String comment;
    String custom;
    bool cover_present;
};
```

---

## Progress Bar Integration

### Simple Progress Bar (Polling)

```cpp
void update_ui_loop() {
    if (player.is_playing()) {
        uint32_t pos_ms = player.current_position_ms();
        uint32_t dur_ms = player.total_duration_ms();

        float progress = (float)pos_ms / dur_ms;  // 0.0 to 1.0
        ui_set_progressbar(progress);

        ui_set_time_label(format_time(pos_ms), format_time(dur_ms));
    }
}
```

### Progress Bar with Callback (Recommended)

```cpp
void on_progress_callback(uint32_t pos_ms, uint32_t dur_ms) {
    // Auto-called every 250ms during playback
    float progress = (float)pos_ms / dur_ms;
    ui_set_progressbar(progress);

    ui_set_time(pos_ms, dur_ms);
}

// Setup
PlayerCallbacks callbacks;
callbacks.on_progress = on_progress_callback;
player.set_callbacks(callbacks);
```

### User Seeks via Progress Bar

```cpp
void on_progressbar_click(float click_position) {
    // click_position: 0.0 to 1.0
    uint32_t total_sec = player.total_duration_sec();
    uint32_t target_sec = (uint32_t)(total_sec * click_position);

    player.request_seek(target_sec);
}
```

---

## Example Usage

### Minimal Music Player

```cpp
AudioPlayer player;

void setup() {
    PlayerCallbacks cb;
    cb.on_metadata = [](const Metadata& m, const char*) {
        display_title(m.title.c_str());
        display_artist(m.artist.c_str());
    };
    cb.on_progress = [](uint32_t pos, uint32_t dur) {
        update_progressbar(pos, dur);
    };
    player.set_callbacks(cb);
}

void play_track(const char* path) {
    if (player.select_source(path) && player.arm_source()) {
        player.start();
    }
}

void update_progressbar(uint32_t pos_ms, uint32_t dur_ms) {
    float progress = (float)pos_ms / dur_ms;
    display_progress(progress);
    display_time(pos_ms / 1000, dur_ms / 1000);
}
```

### Full-Featured Player UI

See [src/main_ui_test.cpp](src/main_ui_test.cpp) for a comprehensive example including:
- ✅ Progress bar with live updates
- ✅ Play/Pause/Stop controls
- ✅ Seek functionality
- ✅ Volume control
- ✅ Metadata display (title, artist, album)
- ✅ Bitrate and format display
- ✅ Multi-format support (MP3, WAV, AAC, FLAC)
- ✅ Error handling
- ✅ State management

---

## Multi-Format Support

The player automatically detects and decodes:

| Format | Extension | Bitrate Calculation | Seek Support |
|--------|-----------|---------------------|--------------|
| **MP3** | `.mp3` | Average (file-based) | ✅ Yes (seek table) |
| **WAV** | `.wav` | PCM formula | ✅ Yes (instant) |
| **AAC** | `.aac`, `.m4a` | Average (file-based) | ❌ No |
| **FLAC** | `.flac` | Average (file-based) | ❌ No |

### Bitrate Formulas

- **MP3/AAC/FLAC**: `(file_size_bytes * 8) / duration_seconds / 1000` (kbps)
- **WAV (PCM)**: `sample_rate * channels * bits_per_sample / 1000` (kbps)

---

## Key Features

### ✅ NEW in This Update

1. **Progress Callback** - Auto-updates every 250ms
   ```cpp
   callbacks.on_progress = [](uint32_t pos, uint32_t dur) { /* ... */ };
   ```

2. **Bitrate Info** - Real-time bitrate display
   ```cpp
   uint32_t kbps = player.current_bitrate();  // e.g., 320 for MP3, 1411 for WAV
   ```

3. **Format Detection** - Automatic multi-format support
   ```cpp
   AudioFormat fmt = player.stream_->format();  // MP3, WAV, AAC, FLAC
   ```

### Complete UI Workflow

```
User Action          Player Method              Callback Triggered
-----------          -------------              ------------------
Select Track    →    select_source()        →
Load Track      →    arm_source()           →   on_metadata()
Press Play      →    start()                →   on_start()
                                            →   on_progress() (every 250ms)
Press Pause     →    toggle_pause()
Press Resume    →    toggle_pause()
Drag Slider     →    request_seek(sec)
Change Volume   →    set_volume(pct)
Track Ends      →    (automatic)            →   on_end()
Error Occurs    →    (automatic)            →   on_error()
Press Stop      →    stop()                 →   on_stop()
```

---

## Testing

Run the comprehensive test suite:

```bash
# Use main_ui_test.cpp as your main file
pio run -t upload
```

The test suite validates:
- ✅ All playback controls
- ✅ Seek functionality
- ✅ Volume control
- ✅ Multi-format playback
- ✅ All UI info methods
- ✅ Error handling
- ✅ Continuous playback with progress

---

## Summary

This API provides **everything** needed for a professional music player UI:

| Feature | Method/Callback | Status |
|---------|----------------|--------|
| **Progress Bar** | `on_progress()`, `current_position_ms()`, `total_duration_ms()` | ✅ |
| **Playback Controls** | `start()`, `stop()`, `toggle_pause()`, `set_pause()` | ✅ |
| **Seek** | `request_seek(seconds)` | ✅ |
| **Volume** | `set_volume()`, `current_volume()` | ✅ |
| **Metadata** | `on_metadata()`, `metadata()` | ✅ |
| **Bitrate** | `current_bitrate()` | ✅ NEW |
| **Format** | `format()` | ✅ |
| **State** | `state()`, `is_playing()` | ✅ |
| **Callbacks** | `on_start`, `on_stop`, `on_end`, `on_error`, `on_metadata`, `on_progress` | ✅ |
| **Multi-Format** | MP3, WAV, AAC, FLAC auto-detection | ✅ |

**No additional code needed** - integrate directly into your UI framework!
