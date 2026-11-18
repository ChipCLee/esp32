# Audio Glitch Fix: Breaks and Noise in Bluetooth Playback

## Problem

When an MP3 plays to a Bluetooth A2DP speaker, the audio breaks up and has
noise. The source file (`half_moon_serenade.mp3`: 44.1 kHz mono, 128 kbps)
sounds clean on a computer, so the fault is in the firmware's playback
path rather than the file.

## Playback path (before)

```
LittleFS ──256 B──▶ Helix decoder ──PCM──▶ ring buffer (4096 frames) ──▶ A2DP callback ──▶ SBC ──▶ speaker
          (Core 1, mp3_dec task, prio 3)              ~93 ms             (Core 0, BT task)
```

## Root causes

| # | Cause | Effect |
|---|-------|--------|
| 1 | The ring buffer holds only 4096 frames (~93 ms), and nothing waits for it to fill before the A2DP callback starts reading | Any decoder, LittleFS, or BT-stack stall over ~90 ms empties the buffer and audio drops out |
| 2 | The decoder reads 256 bytes per pass, taking the mutex each time, and yields with `vTaskDelay(2)` whenever fewer than 1152 frames are free | High overhead per byte and slow refill, so the small buffer is rarely full |
| 3 | On underrun, the callback fades out and resumes as soon as a single frame is available | Many short gaps instead of one clean recovery |
| 4 | The ring buffer is shared between two cores with no memory barriers, and `ring_flush()` resets head/tail and `memset`s the buffer from any task while the other core is reading or writing | The reader can occasionally get stale or partly written frames (clicks/noise), plus glitches around track changes |
| 5 | The single-track loop runs `seek(0)`, which sends the ID3v2 tag (and any album art) back into Helix | A burst of noise or a gap at each loop |
| 6 | The resampler (non-44.1 kHz files only) resets its position on every decoded frame and picks the nearest sample rather than interpolating | Periodic clicks and aliasing noise for 48 kHz / 22.05 kHz files |
| 7 | The volume multiply has no saturation | Wraparound noise if gain ever goes above unity (latent) |
| 8 | Underruns are counted, but the getter returns the count as `out_ring_avail`, and nothing logs it during playback | The problem is hard to diagnose in the field |

## Design

### 1. Larger ring buffer, allocated at runtime

- Remove the static 16 KB array, which frees `.bss`.
- Allocate the ring buffer on the heap the first time a track is opened, after
  Helix has allocated its buffers. Try 16384 frames (64 KB, ~370 ms) first, then
  8192, then 4096, and keep at least 24 KB of heap free for Bluedroid. Log the
  size chosen.
- Size and mask become runtime variables (always a power of two).
- Keep the buffer for the rest of the session. Free it in
  `mp3_player_enter_portal_mode()` so the Wi-Fi web server gets the memory back.

### 2. Prefill and re-buffering

- A `s_buffering` flag starts true. While it is set, the A2DP callback outputs
  silence until the buffer reaches the prefill threshold (half the ring,
  capped at 8192 frames, ~186 ms).
- An underrun still fades out the last sample, then sets `s_buffering` again.
  The result is one clean pause and recovery instead of repeated stutter.
- A flush (new track or stop) also sets `s_buffering`.

### 3. Safe handoff between cores

- Single producer (decoder task, Core 1), single consumer (A2DP callback, Core 0).
- `s_head`, `s_tail` and a generation counter `s_gen` are read and written only
  inside a `portMUX` critical section, which also acts as a memory barrier on
  the ESP32. Each critical section is a few instructions; the frame copy runs
  outside the lock.
- The writer fills frames at a private cursor (`s_w_head`) and publishes
  `s_head` in batches: when the buffer is full, every 256 frames, and at the end
  of each Helix callback.
- The reader snapshots `(head, tail, gen)`, copies frames, then advances `s_tail`
  only if `gen` is unchanged. If a flush happened during the copy, the copied
  frames are replaced with silence.
- **Only the decoder task flushes.** `mp3_player_stop_and_close()` no longer
  calls `ring_flush()`; it sets `s_stop_requested`, and the decoder flushes when
  it sees the request. Because flush is index-only (no `memset`), it costs
  almost nothing.

### 4. Larger, tighter decode loop

- Raise `s_mp3_io_buf` from 256 bytes to 2 KB.
- Decode whenever at least one MP3 frame's worth of space (1152 frames) is free.
  The Helix callback already blocks while the buffer is full.
- When there is no room, sleep 5 ms. With the larger buffer, this cuts
  scheduler churn and still keeps plenty of headroom.
- Raise the decoder task priority from 3 to 5 so OLED and UI work on Core 1
  never delays it. The task blocks most of the time, so Core 1 is not starved.

### 5. Correct track boundaries

- When a file opens, record `s_audio_start` (just past the ID3v2 tag) and
  `s_audio_end` (just before the ID3v1 `TAG` trailer, if present).
- Reads stop at `s_audio_end`, and looping seeks to `s_audio_start` instead of 0.

### 6. Continuous linear-interpolation resampler

- Keep the fractional position and the previous block's last sample between
  callbacks, and interpolate between adjacent samples.
- Reset this state on every flush or new track.
- 44.1 kHz input still uses the fast copy path.

### 7. Saturating volume

- Clamp the scaled sample to `[-32768, 32767]`.

### 8. Diagnostics

- Rename the getter parameter to `out_underruns`.
- While playing, the decoder task logs a line every 5 s: ring size, fill level,
  and new underrun frames since the last log. The OLED `status` serial command
  keeps working.

## Verification

1. Build with `pio run`.
2. Flash and play `half_moon_serenade.mp3` to the speaker.
3. Watch the serial log: `underruns +0` while playing means no gaps.
4. Check that looping a single track is seamless, and that next/prev/pause and
   restarting playback behave as before.

## Out of scope

- SBC bitpool and codec tuning (set by the ESP32-A2DP library and Bluedroid).
- RF range or interference problems. These show up as audible breaks while
  `underruns` stays at 0.
