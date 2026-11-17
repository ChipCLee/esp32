#include "mp3_player.h"
#include "web_server.h"
#include <MP3DecoderHelix.h>

using namespace libhelix;

// 8 KB Lock-Free Stereo PCM Ring Buffer (2048 stereo frames @ 44.1kHz = ~46ms)
#define PCM_RING_FRAMES     2048
#define PCM_RING_MASK       (PCM_RING_FRAMES - 1)

typedef struct __attribute__((packed)) {
    int16_t ch1;
    int16_t ch2;
} pcm_frame_t;

static pcm_frame_t s_pcm_ring[PCM_RING_FRAMES];
static volatile uint32_t s_ring_head = 0;
static volatile uint32_t s_ring_tail = 0;

static mp3_track_t s_tracks[MAX_TRACKS];
static size_t s_track_count = 0;
static int s_current_idx = -1;

static volatile player_state_t s_state = PLAYER_STOPPED;
static volatile bool s_stop_requested = false;
static volatile bool s_file_open = false;
static volatile uint8_t s_volume = 80;       // 0..100
static volatile uint8_t s_vu_left = 0;       // 0..100
static volatile uint8_t s_vu_right = 0;      // 0..100

static char s_cur_path[32]   = {0};
static char s_cur_title[36]  = "No Track Loaded";
static char s_cur_artist[36] = "Upload MP3 @ 192.168.4.1";

static MP3DecoderHelix* s_helix = nullptr;
static File s_audio_file;
static uint8_t s_mp3_io_buf[1024];

static inline uint32_t ring_free_frames() {
    uint32_t used = (s_ring_head - s_ring_tail) & PCM_RING_MASK;
    return (PCM_RING_FRAMES - 1) - used;
}

static inline uint32_t ring_avail_frames() {
    return (s_ring_head - s_ring_tail) & PCM_RING_MASK;
}

static void ring_flush() {
    s_ring_head = 0;
    s_ring_tail = 0;
    memset(s_pcm_ring, 0, sizeof(s_pcm_ring));
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

    int chans = (info.nChans == 1) ? 1 : 2;
    size_t in_frames = len / chans;
    if (in_frames == 0) return;

    uint32_t sr = (info.samprate > 0) ? (uint32_t)info.samprate : 44100;
    // Scale volume quadratically for natural perceptual control
    uint32_t v = s_volume;
    uint32_t vol_q10 = (v * v * 1024) / 10000;

    if (sr == 44100) {
        size_t i = 0;
        while (i < in_frames && !s_stop_requested) {
            if (ring_free_frames() == 0) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            int32_t l = (chans == 1) ? pcm_buffer[i] : pcm_buffer[i * 2];
            int32_t r = (chans == 1) ? l             : pcm_buffer[i * 2 + 1];

            pcm_frame_t out_f;
            out_f.ch1 = (int16_t)((l * (int32_t)vol_q10) >> 10);
            out_f.ch2 = (int16_t)((r * (int32_t)vol_q10) >> 10);

            uint32_t head = s_ring_head;
            s_pcm_ring[head] = out_f;
            s_ring_head = (head + 1) & PCM_RING_MASK;
            i++;
        }
    } else {
        // Simple linear interpolation resampler to 44100 Hz
        double step = (double)sr / 44100.0;
        double pos = 0.0;
        while ((size_t)pos < in_frames && !s_stop_requested) {
            if (ring_free_frames() == 0) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            size_t idx = (size_t)pos;
            int32_t l = (chans == 1) ? pcm_buffer[idx] : pcm_buffer[idx * 2];
            int32_t r = (chans == 1) ? l               : pcm_buffer[idx * 2 + 1];

            pcm_frame_t out_f;
            out_f.ch1 = (int16_t)((l * (int32_t)vol_q10) >> 10);
            out_f.ch2 = (int16_t)((r * (int32_t)vol_q10) >> 10);

            uint32_t head = s_ring_head;
            s_pcm_ring[head] = out_f;
            s_ring_head = (head + 1) & PCM_RING_MASK;
            pos += step;
        }
    }
}

void mp3_player_init() {
    ring_flush();
    // Create decoder instance lazily; buffers (~36 KB) are allocated on begin() during playback
    // so Wi-Fi SoftAP can run at boot within ESP32 DRAM limits.
    s_helix = new MP3DecoderHelix(on_helix_pcm_data);
    mp3_player_refresh_library();
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
    // Stop Wi-Fi SoftAP before allocating MP3DecoderHelix (~36 KB) for playback
    web_server_stop();

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
    s_stop_requested = true;
    s_state = PLAYER_STOPPED;
    ring_flush();
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

// Called from Core 0 by BluetoothA2DPSource to pull 44.1kHz stereo PCM frames
int32_t mp3_player_read_a2dp_frames(uint8_t* frame_buf, int32_t frame_count) {
    if (!frame_buf || frame_count <= 0) return 0;

    pcm_frame_t* frames = (pcm_frame_t*)frame_buf;
    if (s_state != PLAYER_PLAYING) {
        memset(frame_buf, 0, (size_t)frame_count * sizeof(pcm_frame_t));
        s_vu_left = (s_vu_left > 5) ? (s_vu_left - 5) : 0;
        s_vu_right = (s_vu_right > 5) ? (s_vu_right - 5) : 0;
        return frame_count;
    }

    int32_t max_l = 0, max_r = 0;
    int32_t read_cnt = 0;

    while (read_cnt < frame_count && ring_avail_frames() > 0) {
        uint32_t tail = s_ring_tail;
        pcm_frame_t f = s_pcm_ring[tail];
        s_ring_tail = (tail + 1) & PCM_RING_MASK;

        frames[read_cnt++] = f;

        int32_t al = abs((int32_t)f.ch1);
        int32_t ar = abs((int32_t)f.ch2);
        if (al > max_l) max_l = al;
        if (ar > max_r) max_r = ar;
    }

    // Zero-fill any remaining tail frames on underrun
    while (read_cnt < frame_count) {
        frames[read_cnt].ch1 = 0;
        frames[read_cnt].ch2 = 0;
        read_cnt++;
    }

    uint8_t target_l = (uint8_t)constrain((max_l * 100) / 24000, 0, 100);
    uint8_t target_r = (uint8_t)constrain((max_r * 100) / 24000, 0, 100);
    s_vu_left  = (target_l > s_vu_left)  ? target_l : (uint8_t)((s_vu_left * 7 + target_l * 3) / 10);
    s_vu_right = (target_r > s_vu_right) ? target_r : (uint8_t)((s_vu_right * 7 + target_r * 3) / 10);

    return frame_count;
}

void mp3_decoder_task(void* param) {
    (void)param;
    char open_path[32] = {0};

    for (;;) {
        if (s_state != PLAYER_PLAYING || s_stop_requested) {
            if (s_file_open && s_stop_requested) {
                if (fs_mutex && xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    s_audio_file.close();
                    s_file_open = false;
                    open_path[0] = '\0';
                    xSemaphoreGive(fs_mutex);
                }
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
                    if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        strncpy(s_cur_title, t_buf, sizeof(s_cur_title) - 1);
                        strncpy(s_cur_artist, a_buf, sizeof(s_cur_artist) - 1);
                        xSemaphoreGive(state_mutex);
                    }
                    if (s_helix) {
                        s_helix->end();
                        s_helix->begin();
                    }
                    ring_flush();
                } else {
                    s_state = PLAYER_STOPPED;
                }
                xSemaphoreGive(fs_mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Decode next chunk if ring buffer has room
        if (ring_free_frames() >= 1152 && s_file_open && !s_stop_requested) {
            size_t bytes_read = 0;
            bool eof = false;
            if (fs_mutex && xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                if (s_audio_file.available()) {
                    bytes_read = s_audio_file.read(s_mp3_io_buf, sizeof(s_mp3_io_buf));
                } else {
                    eof = true;
                }
                xSemaphoreGive(fs_mutex);
            }

            if (bytes_read > 0 && s_helix) {
                s_helix->write(s_mp3_io_buf, bytes_read);
            } else if (eof) {
                // Close current file and auto-advance to next track (Jukebox mode)
                if (fs_mutex && xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    s_audio_file.close();
                    s_file_open = false;
                    open_path[0] = '\0';
                    xSemaphoreGive(fs_mutex);
                }
                if (s_track_count > 0) {
                    mp3_player_next();
                } else {
                    s_state = PLAYER_STOPPED;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}
