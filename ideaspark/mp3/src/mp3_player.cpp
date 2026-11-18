#include "mp3_player.h"
#include "web_server.h"
#include "bt_manager.h"
#include <MP3DecoderHelix.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

using namespace libhelix;

// Stereo PCM ring buffer (SPSC): producer = mp3_decoder_task (Core 1),
// consumer = A2DP data callback (Core 0). Allocated at runtime once Helix is up,
// trying 16384 frames (~370 ms @ 44.1 kHz), then 8192, then 4096.
// Index updates go through s_ring_mux, which also serves as the cross-core memory barrier.
#define PCM_RING_FRAMES_MAX     16384
#define PCM_RING_FRAMES_MIN     1024
#define PCM_RING_HEAP_RESERVE_BT_IDLE (24 * 1024) // reserve for Bluedroid before BT connects
#define PCM_RING_HEAP_RESERVE_BT_CONN (2 * 1024)  // BT already connected; small safety margin
#define PCM_PREFILL_MAX_FRAMES  8192          // ~186 ms startup / re-buffer latency
#define PCM_PUBLISH_INTERVAL    256           // publish writer head every N frames
#define MP3_FRAME_SAMPLES       1152

typedef struct __attribute__((packed)) {
    int16_t ch1;
    int16_t ch2;
} pcm_frame_t;

static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static pcm_frame_t* s_ring = nullptr;
static uint32_t s_ring_frames = 0;
static uint32_t s_ring_mask = 0;
static uint32_t s_ring_prefill = 0;
static uint32_t s_head = 0;              // published write index (guarded by s_ring_mux)
static uint32_t s_tail = 0;              // read index (guarded by s_ring_mux)
static uint32_t s_gen = 0;               // incremented on each flush (guarded by s_ring_mux)
static volatile bool s_buffering = true; // reader outputs silence until prefill is reached
// Writer-private state (decoder task only)
static uint32_t s_w_head = 0;            // writer cursor; may be ahead of s_head
static uint32_t s_w_free = 0;            // cached free frames as of the last tail snapshot
static uint32_t s_w_unpublished = 0;

// Resampler state (decoder task only); carried between Helix callbacks
static float   s_rs_pos = 0.0f;          // position relative to current block; -1 == previous block's last sample
static int16_t s_rs_prev_l = 0;
static int16_t s_rs_prev_r = 0;

static mp3_track_t s_tracks[MAX_TRACKS];
static size_t s_track_count = 0;
static int s_current_idx = -1;

static volatile player_state_t s_state = PLAYER_STOPPED;
static volatile bool s_stop_requested = false;
static volatile bool s_file_open = false;
static volatile uint8_t s_volume = 80;       // 0..100
static volatile uint8_t s_vu_left = 0;       // 0..100
static volatile uint8_t s_vu_right = 0;      // 0..100
static volatile uint32_t s_dbg_pcm_in = 0;
static volatile uint32_t s_dbg_a2dp_out = 0;
static volatile uint32_t s_dbg_a2dp_calls = 0;
static volatile int32_t  s_dbg_last_fc = 0;
static volatile uint32_t s_dbg_underruns = 0;

static char s_cur_path[32]   = {0};
static char s_cur_title[36]  = "No Track Loaded";
static char s_cur_artist[36] = "Upload MP3 @ 192.168.4.1";

static MP3DecoderHelix* s_helix = nullptr;
static File s_audio_file;
static uint8_t s_mp3_io_buf[2048];
static uint32_t s_audio_start = 0;       // first byte after ID3v2 tag
static uint32_t s_audio_end = 0;         // one past last audio byte (excludes ID3v1 trailer)

// Allocate the ring buffer once (decoder task). Returns true if a buffer is available.
static bool ring_alloc() {
    if (s_ring) return true;
    size_t reserve = bt_manager_is_connected() ? PCM_RING_HEAP_RESERVE_BT_CONN : PCM_RING_HEAP_RESERVE_BT_IDLE;
    for (uint32_t frames = PCM_RING_FRAMES_MAX; frames >= PCM_RING_FRAMES_MIN; frames >>= 1) {
        size_t bytes = frames * sizeof(pcm_frame_t);
        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (largest < bytes || free_heap < bytes + reserve) continue;
        pcm_frame_t* buf = (pcm_frame_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        if (!buf) continue;
        memset(buf, 0, bytes);
        uint32_t prefill = frames / 2;
        if (prefill > PCM_PREFILL_MAX_FRAMES) prefill = PCM_PREFILL_MAX_FRAMES;
        portENTER_CRITICAL(&s_ring_mux);
        s_ring = buf;
        s_ring_frames = frames;
        s_ring_mask = frames - 1;
        s_ring_prefill = prefill;
        s_head = 0;
        s_tail = 0;
        s_gen++;
        portEXIT_CRITICAL(&s_ring_mux);
        s_buffering = true;
        s_w_head = 0;
        s_w_free = 0;
        s_w_unpublished = 0;
        Serial.printf("[MP3] PCM ring: %u frames (%u ms), prefill %u frames, free heap %u\n",
                      (unsigned)frames, (unsigned)(frames * 1000UL / 44100UL),
                      (unsigned)prefill, (unsigned)ESP.getFreeHeap());
        return true;
    }
    Serial.printf("[MP3] PCM ring allocation failed (free=%u, max block=%u)\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return false;
}

// Release the ring buffer. Only call when playback is stopped and the decoder is idle.
static void ring_release() {
    portENTER_CRITICAL(&s_ring_mux);
    pcm_frame_t* buf = s_ring;
    s_ring = nullptr;
    s_ring_frames = 0;
    s_ring_mask = 0;
    s_head = 0;
    s_tail = 0;
    s_gen++;
    portEXIT_CRITICAL(&s_ring_mux);
    s_w_head = 0;
    s_w_free = 0;
    s_w_unpublished = 0;
    if (buf) {
        vTaskDelay(pdMS_TO_TICKS(20)); // let any in-flight A2DP callback finish its copy
        heap_caps_free(buf);
    }
}

// Discard buffered audio. Decoder task only (the sole producer).
static void ring_flush() {
    portENTER_CRITICAL(&s_ring_mux);
    s_head = 0;
    s_tail = 0;
    s_gen++;
    portEXIT_CRITICAL(&s_ring_mux);
    s_buffering = true;
    s_w_head = 0;
    s_w_free = 0;
    s_w_unpublished = 0;
    s_rs_pos = 0.0f;
    s_rs_prev_l = 0;
    s_rs_prev_r = 0;
}

// Writer: refresh the cached free-space count from the reader's tail.
static uint32_t ring_writer_refresh_free() {
    portENTER_CRITICAL(&s_ring_mux);
    uint32_t tail = s_tail;
    portEXIT_CRITICAL(&s_ring_mux);
    if (s_ring_frames == 0) { s_w_free = 0; return 0; }
    uint32_t used = (s_w_head - tail) & s_ring_mask;
    s_w_free = (s_ring_frames - 1) - used;
    return s_w_free;
}

// Writer: make all written frames visible to the reader.
static void ring_publish() {
    if (s_w_unpublished == 0) return;
    portENTER_CRITICAL(&s_ring_mux);
    s_head = s_w_head;
    portEXIT_CRITICAL(&s_ring_mux);
    s_w_unpublished = 0;
}

// Writer: free frames (from the decoder's point of view).
static uint32_t ring_free_frames() {
    return ring_writer_refresh_free();
}

// Writer: append one frame, blocking while the ring is full. Returns false if stop was requested.
static inline bool ring_push(int16_t l, int16_t r) {
    while (s_w_free == 0) {
        ring_publish();
        if (s_stop_requested || !s_ring) return false;
        if (ring_writer_refresh_free() == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    pcm_frame_t& f = s_ring[s_w_head];
    f.ch1 = l;
    f.ch2 = r;
    s_w_head = (s_w_head + 1) & s_ring_mask;
    s_w_free--;
    s_dbg_pcm_in++;
    if (++s_w_unpublished >= PCM_PUBLISH_INTERVAL) ring_publish();
    return true;
}

static inline int16_t apply_volume(int32_t s, int32_t vol_q10) {
    int32_t v = (s * vol_q10) >> 10;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static void filename_to_title(const char* path, char* out_title, size_t title_len) {
    if (!path || !out_title || title_len == 0) return;
    const char* base = strrchr(path, '/');
    base = base ? (base + 1) : path;
    strncpy(out_title, base, title_len - 1);
    out_title[title_len - 1] = '\0';
    char* dot = strrchr(out_title, '.');
    if (dot && strcasecmp(dot, ".mp3") == 0) {
        *dot = '\0';
    }
}

static void trim_trailing_spaces(char* s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\0' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static void read_id3v2_text(File& f, uint32_t frame_size, char* out_buf, size_t out_len) {
    if (frame_size <= 1 || out_len == 0) {
        if (frame_size > 0) f.seek(f.position() + frame_size);
        return;
    }
    uint8_t enc = f.read();
    uint32_t payload = frame_size - 1;

    if (enc == 0 || enc == 3) {
        // ISO-8859-1 or UTF-8
        size_t to_read = (payload < out_len - 1) ? payload : (out_len - 1);
        f.readBytes(out_buf, to_read);
        out_buf[to_read] = '\0';
        if (payload > to_read) {
            f.seek(f.position() + (payload - to_read));
        }
    } else {
        // UTF-16: extract ASCII subset (skip BOM if present)
        size_t idx = 0;
        uint32_t i = 0;
        if (payload >= 2) {
            uint8_t b0 = f.read();
            uint8_t b1 = f.read();
            i += 2;
            if (!((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF))) {
                char c = (b0 != 0) ? (char)b0 : (char)b1;
                if (c >= 0x20 && c <= 0x7E && idx < out_len - 1) out_buf[idx++] = c;
            }
        }
        while (i + 1 < payload) {
            uint8_t b0 = f.read();
            uint8_t b1 = f.read();
            i += 2;
            char c = (b0 != 0) ? (char)b0 : (char)b1;
            if (c == '\0') break;
            if (c >= 0x20 && c <= 0x7E && idx < out_len - 1) {
                out_buf[idx++] = c;
            }
        }
        if (i < payload) f.seek(f.position() + (payload - i));
        out_buf[idx] = '\0';
    }
    trim_trailing_spaces(out_buf);
}

bool mp3_extract_metadata(File& f, const char* path, char* out_title, size_t title_len, char* out_artist, size_t artist_len) {
    out_title[0] = '\0';
    out_artist[0] = '\0';
    uint32_t audio_start_offset = 0;
    size_t fsize = f.size();

    // 1. Check ID3v2 Header ("ID3")
    f.seek(0);
    uint8_t hdr[10];
    if (fsize >= 10 && f.read(hdr, 10) == 10 && hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        uint8_t ver_major = hdr[3];
        uint32_t tag_size = ((uint32_t)(hdr[6] & 0x7F) << 21) |
                            ((uint32_t)(hdr[7] & 0x7F) << 14) |
                            ((uint32_t)(hdr[8] & 0x7F) << 7)  |
                            ((uint32_t)(hdr[9] & 0x7F));
        audio_start_offset = 10 + tag_size;
        uint32_t scan_limit = (tag_size < 4096) ? (10 + tag_size) : (10 + 4096);

        while (f.position() + 10 <= scan_limit && (out_title[0] == '\0' || out_artist[0] == '\0')) {
            uint8_t fh[10];
            if (f.read(fh, 10) != 10) break;
            if (fh[0] == 0) break; // Padding

            uint32_t f_sz = 0;
            if (ver_major >= 4) {
                f_sz = ((uint32_t)(fh[4] & 0x7F) << 21) | ((uint32_t)(fh[5] & 0x7F) << 14) |
                       ((uint32_t)(fh[6] & 0x7F) << 7)  | ((uint32_t)(fh[7] & 0x7F));
            } else {
                f_sz = ((uint32_t)fh[4] << 24) | ((uint32_t)fh[5] << 16) |
                       ((uint32_t)fh[6] << 8)  | ((uint32_t)fh[7]);
            }
            if (f_sz == 0 || f.position() + f_sz > 10 + tag_size) break;

            if (memcmp(fh, "TIT2", 4) == 0 && out_title[0] == '\0') {
                read_id3v2_text(f, f_sz, out_title, title_len);
            } else if (memcmp(fh, "TPE1", 4) == 0 && out_artist[0] == '\0') {
                read_id3v2_text(f, f_sz, out_artist, artist_len);
            } else {
                f.seek(f.position() + f_sz);
            }
        }
    }

    // 2. Fallback to ID3v1 Trailer (last 128 bytes == "TAG")
    if ((out_title[0] == '\0' || out_artist[0] == '\0') && fsize >= 128) {
        f.seek(fsize - 128);
        uint8_t tag[128];
        if (f.read(tag, 128) == 128 && tag[0] == 'T' && tag[1] == 'A' && tag[2] == 'G') {
            if (out_title[0] == '\0') {
                size_t cp = (30 < title_len - 1) ? 30 : (title_len - 1);
                memcpy(out_title, &tag[3], cp);
                out_title[cp] = '\0';
                trim_trailing_spaces(out_title);
            }
            if (out_artist[0] == '\0') {
                size_t cp = (30 < artist_len - 1) ? 30 : (artist_len - 1);
                memcpy(out_artist, &tag[33], cp);
                out_artist[cp] = '\0';
                trim_trailing_spaces(out_artist);
            }
        }
    }

    if (out_title[0] == '\0') {
        filename_to_title(path, out_title, title_len);
    }
    if (out_artist[0] == '\0') {
        strncpy(out_artist, "Local MP3 Track", artist_len - 1);
        out_artist[artist_len - 1] = '\0';
    }

    // Seek file directly to MP3 audio frames (past ID3v2 header/album art)
    if (audio_start_offset < fsize) {
        f.seek(audio_start_offset);
    } else {
        f.seek(0);
    }
    return true;
}

// Helix MP3 PCM Output Callback -> Resamples to 44.1kHz Stereo & pushes to RingBuffer
static void on_helix_pcm_data(MP3FrameInfo &info, int16_t *pcm_buffer, size_t len, void* caller) {
    (void)caller;
    if (len == 0 || s_stop_requested) return;

    if (!s_ring) return;

    int chans = (info.nChans == 1) ? 1 : 2;
    size_t in_frames = len / chans;
    if (in_frames == 0) return;

    uint32_t sr = (info.samprate > 0) ? (uint32_t)info.samprate : 44100;
    // Scale volume quadratically for natural perceptual control
    uint32_t v = s_volume;
    int32_t vol_q10 = (int32_t)((v * v * 1024) / 10000);

    if (sr == 44100) {
        for (size_t i = 0; i < in_frames; ++i) {
            int32_t l = (chans == 1) ? pcm_buffer[i] : pcm_buffer[i * 2];
            int32_t r = (chans == 1) ? l             : pcm_buffer[i * 2 + 1];
            if (!ring_push(apply_volume(l, vol_q10), apply_volume(r, vol_q10))) break;
        }
    } else {
        // Continuous linear-interpolation resampler to 44100 Hz.
        // Index -1 refers to the previous block's last sample, so interpolation
        // and fractional position carry across Helix frame boundaries.
        const float step = (float)sr / 44100.0f;
        const float last = (float)(in_frames - 1);
        float pos = s_rs_pos;
        bool ok = true;
        while (pos < last) {
            int idx = (pos < 0.0f) ? -1 : (int)pos;
            float frac = pos - (float)idx;
            int32_t l0, r0;
            if (idx < 0) {
                l0 = s_rs_prev_l;
                r0 = s_rs_prev_r;
            } else {
                l0 = (chans == 1) ? pcm_buffer[idx] : pcm_buffer[idx * 2];
                r0 = (chans == 1) ? l0              : pcm_buffer[idx * 2 + 1];
            }
            int n = idx + 1;
            int32_t l1 = (chans == 1) ? pcm_buffer[n] : pcm_buffer[n * 2];
            int32_t r1 = (chans == 1) ? l1            : pcm_buffer[n * 2 + 1];
            int32_t l = l0 + (int32_t)((float)(l1 - l0) * frac);
            int32_t r = r0 + (int32_t)((float)(r1 - r0) * frac);
            if (!ring_push(apply_volume(l, vol_q10), apply_volume(r, vol_q10))) { ok = false; break; }
            pos += step;
        }
        if (ok) {
            s_rs_pos = pos - (float)in_frames;   // now in [-1, step - 1)
            size_t li = in_frames - 1;
            s_rs_prev_l = (chans == 1) ? pcm_buffer[li] : pcm_buffer[li * 2];
            s_rs_prev_r = (chans == 1) ? s_rs_prev_l    : pcm_buffer[li * 2 + 1];
        }
    }
    ring_publish();
}

void mp3_player_init() {
    ring_flush();
    // Create decoder instance lazily; buffers (~36 KB) are allocated on begin() during playback
    // so Wi-Fi SoftAP can run at boot within ESP32 DRAM limits.
    s_helix = new MP3DecoderHelix(on_helix_pcm_data);
    mp3_player_refresh_library();

    Preferences prefs;
    prefs.begin("player_cfg", true);
    String sel = prefs.getString("selected", "");
    prefs.end();

    if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int found_idx = -1;
        if (sel.length() > 0) {
            for (size_t i = 0; i < s_track_count; ++i) {
                if (strcmp(s_tracks[i].path, sel.c_str()) == 0) {
                    found_idx = (int)i;
                    break;
                }
            }
        }
        if (found_idx < 0 && s_track_count > 0) {
            found_idx = 0;
        }
        if (found_idx >= 0) {
            s_current_idx = found_idx;
            strncpy(s_cur_path, s_tracks[found_idx].path, sizeof(s_cur_path) - 1);
            strncpy(s_cur_title, s_tracks[found_idx].title, sizeof(s_cur_title) - 1);
            strncpy(s_cur_artist, s_tracks[found_idx].artist, sizeof(s_cur_artist) - 1);
        }
        xSemaphoreGive(state_mutex);
    }
}

void mp3_player_refresh_library() {
    if (fs_mutex && xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return;

    s_track_count = 0;
    File root = LittleFS.open("/");
    if (root && root.isDirectory()) {
        File entry = root.openNextFile();
        while (entry && s_track_count < MAX_TRACKS) {
            if (!entry.isDirectory()) {
                const char* fname = entry.name();
                size_t nlen = strlen(fname);
                if (nlen > 4 && strcasecmp(fname + nlen - 4, ".mp3") == 0) {
                    mp3_track_t& tr = s_tracks[s_track_count];
                    if (fname[0] == '/') {
                        snprintf(tr.path, sizeof(tr.path), "%s", fname);
                    } else {
                        snprintf(tr.path, sizeof(tr.path), "/%s", fname);
                    }
                    tr.size_bytes = entry.size();
                    mp3_extract_metadata(entry, tr.path, tr.title, sizeof(tr.title), tr.artist, sizeof(tr.artist));
                    s_track_count++;
                }
            }
            entry.close();
            entry = root.openNextFile();
        }
        root.close();
    }

    if (fs_mutex) xSemaphoreGive(fs_mutex);
}

size_t mp3_player_get_tracks(mp3_track_t* out_tracks, size_t max_tracks) {
    if (!out_tracks || max_tracks == 0) return 0;
    size_t count = 0;
    if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        count = (s_track_count < max_tracks) ? s_track_count : max_tracks;
        memcpy(out_tracks, s_tracks, count * sizeof(mp3_track_t));
        xSemaphoreGive(state_mutex);
    }
    return count;
}

bool mp3_player_get_track(size_t index, mp3_track_t* out_track) {
    if (!out_track) return false;
    bool ok = false;
    if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (index < s_track_count) {
            *out_track = s_tracks[index];
            ok = true;
        }
        xSemaphoreGive(state_mutex);
    }
    return ok;
}

bool mp3_player_play(const char* path) {
    if (!path || path[0] == '\0') return false;

    mp3_player_stop_and_close();
    // Stop Wi-Fi SoftAP before allocating MP3DecoderHelix (~36 KB) and BT A2DP for playback
    app_set_bluetooth_mode();
    web_server_stop();
    bt_manager_connect_saved();

    Preferences prefs;
    prefs.begin("player_cfg", false);
    prefs.putString("selected", path);
    prefs.end();

    if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        strncpy(s_cur_path, path, sizeof(s_cur_path) - 1);
        s_cur_path[sizeof(s_cur_path) - 1] = '\0';

        // Find index in library
        s_current_idx = -1;
        for (size_t i = 0; i < s_track_count; ++i) {
            if (strcmp(s_tracks[i].path, s_cur_path) == 0) {
                s_current_idx = (int)i;
                strncpy(s_cur_title, s_tracks[i].title, sizeof(s_cur_title) - 1);
                strncpy(s_cur_artist, s_tracks[i].artist, sizeof(s_cur_artist) - 1);
                break;
            }
        }
        if (s_current_idx < 0) {
            filename_to_title(s_cur_path, s_cur_title, sizeof(s_cur_title));
            strncpy(s_cur_artist, "Local MP3 Track", sizeof(s_cur_artist) - 1);
        }
        xSemaphoreGive(state_mutex);
    }

    s_stop_requested = false;
    s_state = PLAYER_PLAYING;
    return true;
}

void mp3_player_play_selected() {
    if (s_cur_path[0] != '\0') {
        mp3_player_play(s_cur_path);
    } else if (s_track_count > 0) {
        mp3_player_play(s_tracks[0].path);
    }
}

void mp3_player_pause() {
    if (s_state == PLAYER_PLAYING) {
        s_state = PLAYER_PAUSED;
        s_vu_left = 0;
        s_vu_right = 0;
    }
}

void mp3_player_resume() {
    if (s_state == PLAYER_PAUSED) {
        s_state = PLAYER_PLAYING;
    }
}

void mp3_player_toggle_pause() {
    if (s_state == PLAYER_PLAYING) {
        mp3_player_pause();
    } else if (s_state == PLAYER_PAUSED) {
        mp3_player_resume();
    } else if (s_state == PLAYER_STOPPED && s_track_count > 0) {
        mp3_player_play(s_tracks[0].path);
    }
}

void mp3_player_stop_and_close() {
    // The decoder task (sole ring producer) flushes the ring when it sees s_stop_requested.
    s_stop_requested = true;
    s_state = PLAYER_STOPPED;
    s_vu_left = 0;
    s_vu_right = 0;

    // Wait up to 300ms for mp3_decoder_task to close s_audio_file safely
    uint32_t t0 = millis();
    while (s_file_open && (millis() - t0) < 300) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void mp3_player_enter_portal_mode() {
    mp3_player_stop_and_close();
    if (s_helix) {
        s_helix->end();
    }
    ring_release();   // return the PCM ring's heap to the Wi-Fi web server
    web_server_start();
}

void mp3_player_next() {
    if (s_track_count == 0) return;
    int next_idx = (s_current_idx + 1) % (int)s_track_count;
    mp3_player_play(s_tracks[next_idx].path);
}

void mp3_player_prev() {
    if (s_track_count == 0) return;
    int prev_idx = (s_current_idx <= 0) ? ((int)s_track_count - 1) : (s_current_idx - 1);
    mp3_player_play(s_tracks[prev_idx].path);
}

void mp3_player_set_volume(uint8_t vol_0_100) {
    if (vol_0_100 > 100) vol_0_100 = 100;
    s_volume = vol_0_100;
}

uint8_t mp3_player_get_volume() {
    return s_volume;
}

player_state_t mp3_player_get_state() {
    return s_state;
}

void mp3_player_get_now_playing(char* out_path, size_t path_len,
                                char* out_title, size_t title_len,
                                char* out_artist, size_t artist_len) {
    if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (out_path && path_len > 0)     { strncpy(out_path, s_cur_path, path_len - 1);     out_path[path_len - 1] = '\0'; }
        if (out_title && title_len > 0)   { strncpy(out_title, s_cur_title, title_len - 1);   out_title[title_len - 1] = '\0'; }
        if (out_artist && artist_len > 0) { strncpy(out_artist, s_cur_artist, artist_len - 1); out_artist[artist_len - 1] = '\0'; }
        xSemaphoreGive(state_mutex);
    }
}

void mp3_player_get_vu(uint8_t* out_left, uint8_t* out_right) {
    if (out_left)  *out_left  = s_vu_left;
    if (out_right) *out_right = s_vu_right;
}

void mp3_player_get_debug_stats(uint32_t* out_pcm_in, uint32_t* out_a2dp_out, uint32_t* out_underruns) {
    if (out_pcm_in)    *out_pcm_in    = s_dbg_pcm_in;
    if (out_a2dp_out)  *out_a2dp_out  = s_dbg_a2dp_out;
    if (out_underruns) *out_underruns = s_dbg_underruns;
}

// Called from Core 0 by BluetoothA2DPSource to pull 44.1kHz stereo PCM frames
int32_t mp3_player_read_a2dp_frames(uint8_t* frame_buf, int32_t frame_count) {
    if (!frame_buf || frame_count <= 0) return 0;
    s_dbg_a2dp_out += (uint32_t)frame_count;
    s_dbg_a2dp_calls++;
    s_dbg_last_fc = frame_count;

    pcm_frame_t* frames = (pcm_frame_t*)frame_buf;
    static int16_t s_last_l = 0, s_last_r = 0;

    // Snapshot ring indices (critical section doubles as cross-core memory barrier)
    portENTER_CRITICAL(&s_ring_mux);
    pcm_frame_t* ring = s_ring;
    uint32_t mask = s_ring_mask;
    uint32_t head = s_head;
    uint32_t tail = s_tail;
    uint32_t gen  = s_gen;
    uint32_t prefill = s_ring_prefill;
    portEXIT_CRITICAL(&s_ring_mux);

    uint32_t avail = ring ? ((head - tail) & mask) : 0;
    if (ring && s_buffering && avail >= prefill) {
        s_buffering = false;
    }

    if (s_state != PLAYER_PLAYING || !ring || s_buffering) {
        memset(frame_buf, 0, (size_t)frame_count * sizeof(pcm_frame_t));
        s_last_l = 0;
        s_last_r = 0;
        s_vu_left = (s_vu_left > 5) ? (s_vu_left - 5) : 0;
        s_vu_right = (s_vu_right > 5) ? (s_vu_right - 5) : 0;
        return frame_count;
    }

    int32_t max_l = 0, max_r = 0;
    int32_t read_cnt = (avail < (uint32_t)frame_count) ? (int32_t)avail : frame_count;

    // Copy outside the lock (handles wraparound)
    uint32_t first = mask + 1 - tail;
    if (first > (uint32_t)read_cnt) first = (uint32_t)read_cnt;
    memcpy(frames, &ring[tail], first * sizeof(pcm_frame_t));
    if ((uint32_t)read_cnt > first) {
        memcpy(&frames[first], &ring[0], ((uint32_t)read_cnt - first) * sizeof(pcm_frame_t));
    }

    // Commit tail only if no flush happened during the copy
    bool valid;
    portENTER_CRITICAL(&s_ring_mux);
    valid = (gen == s_gen);
    if (valid) s_tail = (tail + (uint32_t)read_cnt) & mask;
    portEXIT_CRITICAL(&s_ring_mux);
    if (!valid) {
        memset(frame_buf, 0, (size_t)frame_count * sizeof(pcm_frame_t));
        s_last_l = 0;
        s_last_r = 0;
        return frame_count;
    }

    for (int32_t i = 0; i < read_cnt; ++i) {
        int32_t al = abs((int32_t)frames[i].ch1);
        int32_t ar = abs((int32_t)frames[i].ch2);
        if (al > max_l) max_l = al;
        if (ar > max_r) max_r = ar;
    }
    if (read_cnt > 0) {
        s_last_l = frames[read_cnt - 1].ch1;
        s_last_r = frames[read_cnt - 1].ch2;
    }

    // Underrun: fade the last sample out, then re-buffer to prefill before resuming
    if (read_cnt < frame_count) {
        s_dbg_underruns = s_dbg_underruns + (uint32_t)(frame_count - read_cnt);
        s_buffering = true;
        while (read_cnt < frame_count) {
            s_last_l = (int16_t)(((int32_t)s_last_l * 15) >> 4);
            s_last_r = (int16_t)(((int32_t)s_last_r * 15) >> 4);
            frames[read_cnt].ch1 = s_last_l;
            frames[read_cnt].ch2 = s_last_r;
            read_cnt++;
        }
    }

    uint8_t target_l = (uint8_t)constrain((max_l * 100) / 24000, 0, 100);
    uint8_t target_r = (uint8_t)constrain((max_r * 100) / 24000, 0, 100);
    s_vu_left  = (target_l > s_vu_left)  ? target_l : (uint8_t)((s_vu_left * 7 + target_l * 3) / 10);
    s_vu_right = (target_r > s_vu_right) ? target_r : (uint8_t)((s_vu_right * 7 + target_r * 3) / 10);

    return frame_count;
}

// Writer-side view of how many frames the reader still has queued.
static uint32_t ring_queued_frames() {
    portENTER_CRITICAL(&s_ring_mux);
    uint32_t used = s_ring ? ((s_head - s_tail) & s_ring_mask) : 0;
    portEXIT_CRITICAL(&s_ring_mux);
    return used;
}

// Record audio byte range: skip ID3v2 header (metadata parser leaves file there)
// and exclude a trailing 128-byte ID3v1 "TAG" block. Caller holds fs_mutex.
static void detect_audio_bounds(File& f) {
    uint32_t start = (uint32_t)f.position();
    uint32_t size  = (uint32_t)f.size();
    uint32_t end   = size;
    if (size >= 128 && size - 128 >= start) {
        uint8_t tag[3] = {0};
        f.seek(size - 128);
        if (f.read(tag, 3) == 3 && tag[0] == 'T' && tag[1] == 'A' && tag[2] == 'G') {
            end = size - 128;
        }
    }
    if (start >= end) start = 0;
    f.seek(start);
    s_audio_start = start;
    s_audio_end = end;
}

// At the end of a track, let the reader play out what is buffered before moving on.
// Pauses do not count toward the timeout; stop requests abort immediately.
static void drain_ring_before_advance() {
    if (s_helix) s_helix->flush();
    ring_publish();
    s_buffering = false;   // play the tail even if below the prefill threshold
    uint32_t waited_ms = 0;
    while (!s_stop_requested && ring_queued_frames() > 0 && waited_ms < 3000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (s_state == PLAYER_PLAYING) waited_ms += 10;
    }
}

void mp3_decoder_task(void* param) {
    (void)param;
    char open_path[32] = {0};
    uint32_t last_log_ms = millis();
    uint32_t last_log_underruns = s_dbg_underruns;
    uint32_t last_log_a2dp = s_dbg_a2dp_out;
    uint32_t last_log_calls = s_dbg_a2dp_calls;

    for (;;) {
        if (s_state != PLAYER_PLAYING || s_stop_requested) {
            if (s_stop_requested) {
                if (s_file_open && fs_mutex && xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    s_audio_file.close();
                    s_file_open = false;
                    open_path[0] = '\0';
                    xSemaphoreGive(fs_mutex);
                }
                // This task is the only ring producer, so the flush happens here, not in the caller.
                ring_flush();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Open target file if not yet opened or track changed
        if (!s_file_open || strcmp(open_path, s_cur_path) != 0) {
            if (fs_mutex && xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                if (s_file_open) {
                    s_audio_file.close();
                    s_file_open = false;
                }
                s_audio_file = LittleFS.open(s_cur_path, "r");
                if (s_audio_file && !s_audio_file.isDirectory()) {
                    s_file_open = true;
                    strncpy(open_path, s_cur_path, sizeof(open_path) - 1);
                    char t_buf[36], a_buf[36];
                    mp3_extract_metadata(s_audio_file, s_cur_path, t_buf, sizeof(t_buf), a_buf, sizeof(a_buf));
                    detect_audio_bounds(s_audio_file);
                    if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        strncpy(s_cur_title, t_buf, sizeof(s_cur_title) - 1);
                        strncpy(s_cur_artist, a_buf, sizeof(s_cur_artist) - 1);
                        xSemaphoreGive(state_mutex);
                    }
                    bool helix_ok = true;
                    if (s_helix) {
                        s_helix->end();
                        helix_ok = s_helix->begin();
                        if (!helix_ok) {
                            Serial.printf("[MP3] Helix begin() failed (free=%u, max=%u)\n",
                                          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
                            s_helix->end();
                        }
                    }
                    if (helix_ok && ring_alloc()) {
                        ring_flush();
                    } else {
                        if (s_helix) s_helix->end();
                        s_audio_file.close();
                        s_file_open = false;
                        open_path[0] = '\0';
                        s_state = PLAYER_STOPPED;
                    }
                } else {
                    s_state = PLAYER_STOPPED;
                }
                xSemaphoreGive(fs_mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Periodic playback diagnostics
        uint32_t now = millis();
        if (now - last_log_ms >= 5000UL) {
            uint32_t dt = now - last_log_ms;
            uint32_t u = s_dbg_underruns;
            uint32_t a = s_dbg_a2dp_out;
            uint32_t c = s_dbg_a2dp_calls;
            uint32_t a2dp_fps = (dt > 0) ? (uint32_t)(((uint64_t)(a - last_log_a2dp) * 1000ULL) / dt) : 0;
            Serial.printf("[MP3] ring %u/%u, underruns +%u (%u), a2dp %u fps (%u calls, fc=%d)%s\n",
                          (unsigned)ring_queued_frames(), (unsigned)s_ring_frames,
                          (unsigned)(u - last_log_underruns), (unsigned)u,
                          (unsigned)a2dp_fps, (unsigned)(c - last_log_calls), (int)s_dbg_last_fc,
                          s_buffering ? " [buffering]" : "");
            last_log_underruns = u;
            last_log_a2dp = a;
            last_log_calls = c;
            last_log_ms = now;
        }

        // Decode while there is room for at least one more MP3 frame of PCM
        if (ring_free_frames() >= MP3_FRAME_SAMPLES && s_file_open && !s_stop_requested) {
            size_t bytes_read = 0;
            bool eof = false;
            if (fs_mutex && xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                uint32_t pos = (uint32_t)s_audio_file.position();
                if (pos < s_audio_end) {
                    size_t want = sizeof(s_mp3_io_buf);
                    if (want > s_audio_end - pos) want = s_audio_end - pos;
                    bytes_read = s_audio_file.read(s_mp3_io_buf, want);
                }
                if (bytes_read == 0) eof = true;
                xSemaphoreGive(fs_mutex);
            }

            if (bytes_read > 0 && s_helix) {
                s_helix->write(s_mp3_io_buf, bytes_read);
            } else if (eof) {
                if (s_track_count <= 1) {
                    // Seamless single-track loop: restart at the first audio frame, not the ID3 tag
                    if (fs_mutex && xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                        s_audio_file.seek(s_audio_start);
                        xSemaphoreGive(fs_mutex);
                    }
                    if (s_helix) s_helix->flush();
                } else {
                    // Play out the buffered tail, then close and auto-advance the playlist
                    drain_ring_before_advance();
                    if (s_stop_requested) continue;
                    if (fs_mutex && xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                        s_audio_file.close();
                        s_file_open = false;
                        open_path[0] = '\0';
                        xSemaphoreGive(fs_mutex);
                    }
                    mp3_player_next();
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}
