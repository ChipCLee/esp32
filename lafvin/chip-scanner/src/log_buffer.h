#pragma once

#include "types.h"

void log_buffer_init();
void log_buffer_push(rf_protocol_t proto, const char* fmt, ...);
void log_buffer_clear();

// Browser-assisted wall-clock RTC synchronization (Section 3.4)
void log_sync_epoch(uint32_t epoch_s);
bool log_is_rtc_synced();

// Format a log entry timestamp as "[HH:MM:SS]" or "[+HH:MM:SS]"
void log_format_timestamp(const lcd_log_entry_t& entry, char* out_buf, size_t out_len);

// Thread-safe copy of the most recent N log entries (ordered oldest -> newest)
size_t log_buffer_get_recent(lcd_log_entry_t* out_entries, size_t max_entries);
uint32_t log_buffer_get_revision();

