#include "sd_logger.h"
#include "device_table.h"
#include "log_buffer.h"
#include <SD.h>
#include <FS.h>
#include <freertos/queue.h>

#define SD_LOG_FILE "/discovery.csv"
static SPIClass* s_spi = nullptr;
static bool s_sd_mounted = false;
static QueueHandle_t s_sd_queue = nullptr;

static bool try_mount_sd() {
    if (!s_spi) return false;
    if (s_sd_mounted) return true;

    if (SD.begin(PIN_SD_CS, *s_spi, 16000000)) {
        if (SD.cardType() != CARD_NONE) {
            s_sd_mounted = true;
            if (!SD.exists(SD_LOG_FILE)) {
                File f = SD.open(SD_LOG_FILE, FILE_WRITE);
                if (f) {
                    f.println("timestamp_s,protocol,mac_or_id,name,rssi,channel,details");
                    f.close();
                }
            }
            return true;
        }
    }
    return false;
}

void sd_logger_init(SPIClass* shared_spi) {
    s_spi = shared_spi;
    s_sd_queue = xQueueCreate(24, sizeof(discovered_device_t));

    if (spi_bus_mutex && xSemaphoreTake(spi_bus_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        bool ok = try_mount_sd();
        xSemaphoreGive(spi_bus_mutex);
        if (ok) {
            log_buffer_push(PROTO_SYSTEM, "Micro-SD card mounted OK");
        } else {
            log_buffer_push(PROTO_SYSTEM, "SD card absent (RAM mode)");
        }
    }
}

bool sd_logger_is_ready() {
    return s_sd_mounted;
}

void sd_logger_enqueue(const discovered_device_t& dev) {
    if (!s_sd_queue) return;
    xQueueSend(s_sd_queue, &dev, 0);
}

void sd_logger_task(void* param) {
    (void)param;
    discovered_device_t item;
    uint32_t last_remount_check_ms = 0;

    for (;;) {
        if (xQueueReceive(s_sd_queue, &item, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (spi_bus_mutex && xSemaphoreTake(spi_bus_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (!s_sd_mounted) {
                    uint32_t now = millis();
                    if (now - last_remount_check_ms > 10000) {
                        last_remount_check_ms = now;
                        try_mount_sd();
                    }
                }

                if (s_sd_mounted) {
                    File f = SD.open(SD_LOG_FILE, FILE_APPEND);
                    if (f) {
                        char id_str[28];
                        format_mac_or_id(item.protocol, item.mac_or_id, id_str, sizeof(id_str));
                        uint32_t ts = log_is_rtc_synced() ? (uint32_t)time(nullptr) : (item.last_seen_ms / 1000UL);
                        f.printf("%lu,%s,\"%s\",\"%s\",%d,%u,\"%s\"\n",
                                 (unsigned long)ts,
                                 protocol_to_str(item.protocol),
                                 id_str,
                                 item.name,
                                 (int)item.rssi,
                                 (unsigned)item.channel,
                                 item.details);
                        f.close();
                    } else {
                        // Handle SD card hot-unplug gracefully (NFR-8, TC-07)
                        SD.end();
                        s_sd_mounted = false;
                    }
                }
                xSemaphoreGive(spi_bus_mutex);
            }
        }
    }
}

String sd_logger_export_csv() {
    String csv;
    csv.reserve(4096);

    // First try reading from SD card if mounted
    bool read_from_sd = false;
    if (s_sd_mounted && spi_bus_mutex && xSemaphoreTake(spi_bus_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        File f = SD.open(SD_LOG_FILE, FILE_READ);
        if (f) {
            // Read last 8 KB max so we don't exhaust heap
            if (f.size() > 8192) {
                f.seek(f.size() - 8192);
                f.readStringUntil('\n'); // align to next line
                csv += "timestamp_s,protocol,mac_or_id,name,rssi,channel,details\n";
            }
            while (f.available()) {
                csv += (char)f.read();
            }
            f.close();
            read_from_sd = true;
        } else {
            s_sd_mounted = false;
        }
        xSemaphoreGive(spi_bus_mutex);
    }

    if (read_from_sd && csv.length() > 0) {
        return csv;
    }

    // Fallback: export current in-memory discovery table as CSV (NFR-8 graceful degradation)
    csv = "timestamp_s,protocol,mac_or_id,name,rssi,channel,details\n";
    static discovered_device_t snap[MAX_DISCOVERED_DEVICES];
    size_t count = device_table_snapshot(snap, MAX_DISCOVERED_DEVICES);
    for (size_t i = 0; i < count; ++i) {
        char id_str[28];
        format_mac_or_id(snap[i].protocol, snap[i].mac_or_id, id_str, sizeof(id_str));
        char line[180];
        snprintf(line, sizeof(line), "%lu,%s,\"%s\",\"%s\",%d,%u,\"%s\"\n",
                 (unsigned long)(snap[i].last_seen_ms / 1000UL),
                 protocol_to_str(snap[i].protocol),
                 id_str,
                 snap[i].name,
                 (int)snap[i].rssi,
                 (unsigned)snap[i].channel,
                 snap[i].details);
        csv += line;
    }
    return csv;
}

