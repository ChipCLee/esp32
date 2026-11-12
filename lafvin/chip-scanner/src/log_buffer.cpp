#include "log_buffer.h"
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>

static lcd_log_entry_t s_log_ring[LCD_LOG_HISTORY_SIZE];
static uint8_t s_log_head = 0;
static uint8_t s_log_count = 0;
static uint32_t s_log_revision = 0;
static bool s_rtc_synced = false;

void log_buffer_init() {
    memset(s_log_ring, 0, sizeof(s_log_ring));
    s_log_head = 0;
    s_log_count = 0;
    s_log_revision = 1;
}

void log_sync_epoch(uint32_t epoch_s) {
    if (epoch_s < 1700000000UL) return; // Ignore invalid epochs
    struct timeval tv;
    tv.tv_sec = (time_t)epoch_s;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    bool was_synced = s_rtc_synced;
    s_rtc_synced = true;
    if (!was_synced) {
        log_buffer_push(PROTO_SYSTEM, "RTC clock synced via Web");
    }
}

bool log_is_rtc_synced() {
    return s_rtc_synced;
}

void log_buffer_push(rf_protocol_t proto, const char* fmt, ...) {
    char msg_buf[38];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);
    msg_buf[sizeof(msg_buf) - 1] = '\0';

    if (data_table_mutex && xSemaphoreTake(data_table_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        lcd_log_entry_t& entry = s_log_ring[s_log_head];
        entry.protocol = proto;
        entry.is_wall_clock = s_rtc_synced;
        if (s_rtc_synced) {
            time_t now = time(nullptr);
            entry.timestamp_s = (uint32_t)now;
        } else {
            entry.timestamp_s = millis() / 1000UL;
        }
        strncpy(entry.message, msg_buf, sizeof(entry.message) - 1);
        entry.message[sizeof(entry.message) - 1] = '\0';

        s_log_head = (s_log_head + 1) % LCD_LOG_HISTORY_SIZE;
        if (s_log_count < LCD_LOG_HISTORY_SIZE) {
            s_log_count++;
        }
        s_log_revision++;
        xSemaphoreGive(data_table_mutex);
    }
}

void log_buffer_clear() {
    if (data_table_mutex && xSemaphoreTake(data_table_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memset(s_log_ring, 0, sizeof(s_log_ring));
        s_log_head = 0;
        s_log_count = 0;
        s_log_revision++;
        xSemaphoreGive(data_table_mutex);
    }
    log_buffer_push(PROTO_SYSTEM, "Activity log buffer cleared");
}

void log_format_timestamp(const lcd_log_entry_t& entry, char* out_buf, size_t out_len) {
    if (entry.is_wall_clock) {
        time_t raw = (time_t)entry.timestamp_s;
        struct tm ti;
        localtime_r(&raw, &ti);
        snprintf(out_buf, out_len, "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
    } else {
        uint32_t s = entry.timestamp_s;
        uint32_t hrs = (s / 3600UL) % 100UL;
        uint32_t mins = (s % 3600UL) / 60UL;
        uint32_t secs = s % 60UL;
        snprintf(out_buf, out_len, "+%02lu:%02lu:%02lu",
                 (unsigned long)hrs, (unsigned long)mins, (unsigned long)secs);
    }
}

size_t log_buffer_get_recent(lcd_log_entry_t* out_entries, size_t max_entries) {
    if (!out_entries || max_entries == 0) return 0;
    size_t copied = 0;
    if (data_table_mutex && xSemaphoreTake(data_table_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        size_t count = (s_log_count < max_entries) ? s_log_count : max_entries;
        uint8_t start_idx = (s_log_head + LCD_LOG_HISTORY_SIZE - count) % LCD_LOG_HISTORY_SIZE;
        for (size_t i = 0; i < count; ++i) {
            uint8_t idx = (start_idx + i) % LCD_LOG_HISTORY_SIZE;
            out_entries[i] = s_log_ring[idx];
        }
        copied = count;
        xSemaphoreGive(data_table_mutex);
    }
    return copied;
}

uint32_t log_buffer_get_revision() {
    return s_log_revision;
}

