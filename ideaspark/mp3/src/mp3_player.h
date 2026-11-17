#pragma once

#include "types.h"
#include <LittleFS.h>

void mp3_player_init();
void mp3_decoder_task(void* param);

// Library management (thread-safe via fs_mutex)
void mp3_player_refresh_library();
size_t mp3_player_get_tracks(mp3_track_t* out_tracks, size_t max_tracks);
bool mp3_player_get_track(size_t index, mp3_track_t* out_track);
bool mp3_extract_metadata(File& f, const char* path, char* out_title, size_t title_len, char* out_artist, size_t artist_len);

// Transport controls
bool mp3_player_play(const char* path);
void mp3_player_play_selected();
void mp3_player_pause();
void mp3_player_resume();
void mp3_player_toggle_pause();
void mp3_player_stop_and_close();
void mp3_player_enter_portal_mode();
void mp3_player_next();
void mp3_player_prev();
void mp3_player_set_volume(uint8_t vol_0_100);
uint8_t mp3_player_get_volume();

// Status & VU meter getters
player_state_t mp3_player_get_state();
void mp3_player_get_now_playing(char* out_path, size_t path_len,
                                char* out_title, size_t title_len,
                                char* out_artist, size_t artist_len);
void mp3_player_get_vu(uint8_t* out_left, uint8_t* out_right);
void mp3_player_get_debug_stats(uint32_t* out_pcm_in, uint32_t* out_a2dp_out, uint32_t* out_ring_avail);

// Called by BluetoothA2DPSource audio callback on Core 0
int32_t mp3_player_read_a2dp_frames(uint8_t* frame_buf, int32_t frame_count);
