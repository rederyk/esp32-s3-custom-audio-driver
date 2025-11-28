# Analysis of Timeshift Stream Failure with 4 Ready Chunks

## Problem Summary
The stream fails with:
```
[DEBUG] Live stream: no data available, rewinding by 2 chunks (attempt 1)...
[INFO]  Rewind limited to 1 chunks (50% of buffer)
[INFO]  Rewound 1 chunks: chunk 0, offset 48018, time 0 ms (attempt 1)
...
[ERROR] Rewind failed: Exceeded maximum attempts (2)
[INFO]  Could not rewind chunks, ending live stream playback.
```

Despite having 4 ready chunks, the system terminates playback after only 2 rewind attempts.

## Root Causes Identified
1. **Too aggressive rewind failure threshold**: `MAX_REWIND_ATTEMPTS = 2` → too low for variable network conditions.
2. **Preloader too aggressive**: `MAX_FAILED_ATTEMPTS = 3` triggers fallback rewind prematurely.
3. **Tight live edge detection**: Only 4KB tolerance before triggering wait/rewind.
4. **Small cleanup safe zone**: Protects only current +2 chunks.
5. **Short timeouts**: 3s waits, 30s stream timeout insufficient for slow connections.
6. **Conservative rewind limits**: Limited to 50% of buffer chunks.

## Fixes Applied
| Parameter | Original | New | Impact |
|-----------|----------|-----|--------|
| `MAX_FAILED_ATTEMPTS` | 3 | 5 | More resilient preloading |
| `MAX_REWIND_ATTEMPTS` | 2 | 5 | Allows more recovery attempts |
| Live edge tolerance | 4096B | 131072B (128KB) | Handles larger gaps |
| Gap tolerance in `find_chunk_for_offset` | 4096B | 131072B | Better chunk matching |
| Cleanup safe zone | +2 chunks | +4 chunks | Protects more future data |
| Rewind limit | 50% buffer | 75% buffer | Allows deeper recovery |
| Wait timeouts | 3s | 10s | Longer buffering patience |
| Stream timeout | 30s | 60s | Better slow connection handling |

## Expected Behavior Post-Fix
- Preloader waits longer before fallback rewind.
- Rewind attempts increased to 5.
- Larger tolerances prevent premature failure.
- System should sustain playback even during network hiccups with 4+ chunks ready.

## Testing Recommendation
Run: `./pio_run.sh` or `pio run -t upload monitor`
Test with slow/unstable WiFi and the same stream `http://srock.co.za/radio320`.
Monitor for sustained playback beyond previous failure point.

## Code Flow Validation
1. Download fills chunks → promote to READY.
2. Playback reads → preloader anticipates next chunk.
3. If gap detected → wait/rewind with increased tolerance.
4. Cleanup protects larger safe zone.
5. Recovery mechanisms kick in before EOF.

Stream should now handle the scenario robustly.
