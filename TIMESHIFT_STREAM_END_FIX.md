# Timeshift Stream End & Pause/Resume Fix

## Problem Analysis

The user reported an issue where:
1. When the audio stream ends, the buffer continues creating new chunks but playback doesn't pick them up
2. When pause is called, the buffer state becomes inconsistent
3. After resuming from pause, playback doesn't properly reconnect to the buffer

### Root Cause

The issue was in `src/audio_player.cpp` in the main playback loop's end-of-stream handling. There were **two conflicting `vTaskDelay` calls** that created a logic error:

```cpp
if (ts && ts->is_running()) {
    LOG_DEBUG("Live stream: no data available, waiting for next chunk...");
    vTaskDelay(pdMS_TO_TICKS(3000));  // 3 second delay
    continue;
    // UNREACHABLE CODE BELOW:
    vTaskDelay(pdMS_TO_TICKS(50));    // 50ms delay (never executed)
    continue;
}
```

**Impact:**
- The playback loop was waiting **3 seconds** between each retry when no data was available
- This caused the buffer to stall and chunks to be cleaned up before playback could consume them
- The 50ms delay (which would have been responsive) was unreachable dead code
- When pause was called, the recording paused but playback continued trying to read, causing buffer inconsistency

## Solution

### Fix Applied

Removed the unreachable 3-second delay and kept only the responsive 50ms delay:

```cpp
if (ts && ts->is_running()) {
    LOG_DEBUG("Live stream: no data available, waiting for next chunk...");
    // Use a shorter, more responsive delay to avoid getting stuck.
    // This allows the task to yield and quickly re-check for data,
    // making it more resilient to temporary buffer underruns in live streams.
    vTaskDelay(pdMS_TO_TICKS(50));
    continue; // Re-enter the loop to try reading again
}
```

### Why This Works

1. **Responsive Retry Loop**: With a 50ms delay instead of 3 seconds, the playback task checks for new chunks much more frequently
2. **Buffer Consistency**: Chunks are less likely to be cleaned up before playback can consume them
3. **Pause/Resume Safety**: The shorter delay means the playback loop is more responsive to pause/resume state changes
4. **Live Stream Resilience**: The system can now handle temporary buffer underruns gracefully without stalling

## How Pause/Resume Works

The pause/resume mechanism is already properly implemented:

1. **Pause** (`toggle_pause()` / `set_pause(true)`):
   - Sets `pause_flag_ = true`
   - Calls `ts->pause_recording()` to stop the download task
   - Playback loop enters pause state and waits

2. **Resume** (`set_pause(false)`):
   - Sets `pause_flag_ = false`
   - Calls `ts->resume_recording()` to restart the download task
   - Playback loop resumes reading from the buffer

The fix ensures that when playback resumes, it can quickly reconnect to the buffer without the 3-second stall.

## Testing Recommendations

1. **Test Normal Playback**: Verify that live streams play smoothly without stalling
2. **Test Pause/Resume**: 
   - Pause playback
   - Wait a few seconds
   - Resume playback
   - Verify smooth continuation without gaps
3. **Test Stream End**: 
   - Let a stream play to completion
   - Verify that playback ends cleanly without hanging
4. **Test Buffer Cleanup**: 
   - Monitor logs to ensure chunks are being cleaned up appropriately
   - Verify that playback doesn't get interrupted by cleanup

## Files Modified

- `src/audio_player.cpp`: Fixed the end-of-stream retry logic in `audio_task()` method

## Related Code

The timeshift manager's pause/resume mechanism:
- `TimeshiftManager::pause_recording()`: Sets `pause_download_ = true`
- `TimeshiftManager::resume_recording()`: Sets `pause_download_ = false`
- Download task checks this flag and pauses/resumes accordingly

The audio player's pause/resume mechanism:
- `AudioPlayer::toggle_pause()`: Toggles pause state and calls timeshift pause/resume
- `AudioPlayer::set_pause(bool)`: Explicitly sets pause state
- Playback loop respects `pause_flag_` and waits when paused
