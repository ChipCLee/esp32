#include "device_table.h"
#include "log_buffer.h"
#include "sd_logger.h"

static discovered_device_t s_devices[MAX_DISCOVERED_DEVICES];
static size_t s_device_count = 0;
static uint32_t s_table_revision = 1;

void device_table_init() {
    memset(s_devices, 0, sizeof(s_devices));
    s_device_count = 0;
    s_table_revision = 1;
}

void device_table_clear() {
    if (data_table_mutex && xSemaphoreTake(data_table_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(s_devices, 0, sizeof(s_devices));
        s_device_count = 0;
        s_table_revision++;
        xSemaphoreGive(data_table_mutex);
    }
    log_buffer_push(PROTO_SYSTEM, "Device table cleared");
}

void format_mac_or_id(rf_protocol_t proto, const uint8_t* id, char* out_str, size_t out_len) {
    if (proto == PROTO_ZIGBEE) {
        // If upper bytes are non-zero, format 8-byte ExtPAN or short PAN/SrcAddr
        if (id[2] == 0 && id[3] == 0 && id[4] == 0 && id[5] == 0) {
            uint16_t pan_id = ((uint16_t)id[0] << 8) | id[1];
            uint16_t src_addr = ((uint16_t)id[6] << 8) | id[7];
            snprintf(out_str, out_len, "PAN:0x%04X/0x%04X", pan_id, src_addr);
        } else {
            snprintf(out_str, out_len, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                     id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
        }
    } else {
        snprintf(out_str, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
                 id[0], id[1], id[2], id[3], id[4], id[5]);
    }
}

bool device_table_upsert(rf_protocol_t protocol,
                         const uint8_t* mac_or_id,
                         size_t id_len,
                         const char* name,
                         int8_t rssi,
                         uint8_t channel,
                         const char* details,
                         uint8_t ble_addr_type,
                         bool ble_connectable) {
    if (!mac_or_id || id_len == 0) return false;
    uint8_t norm_id[8] = {0};
    size_t copy_len = (id_len > 8) ? 8 : id_len;
    memcpy(norm_id, mac_or_id, copy_len);

    bool is_new = false;
    discovered_device_t log_copy;
    memset(&log_copy, 0, sizeof(log_copy));

    if (data_table_mutex && xSemaphoreTake(data_table_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        int found_idx = -1;
        for (size_t i = 0; i < s_device_count; ++i) {
            if (s_devices[i].protocol == protocol &&
                memcmp(s_devices[i].mac_or_id, norm_id, 8) == 0) {
                found_idx = (int)i;
                break;
            }
        }

        size_t target_idx = 0;
        if (found_idx >= 0) {
            target_idx = (size_t)found_idx;
        } else {
            is_new = true;
            if (s_device_count < MAX_DISCOVERED_DEVICES) {
                target_idx = s_device_count++;
            } else {
                // Evict the oldest unseen device in the fixed 100-item pool (Section 3.6)
                uint32_t oldest_ms = s_devices[0].last_seen_ms;
                target_idx = 0;
                for (size_t i = 1; i < MAX_DISCOVERED_DEVICES; ++i) {
                    if (s_devices[i].last_seen_ms < oldest_ms) {
                        oldest_ms = s_devices[i].last_seen_ms;
                        target_idx = i;
                    }
                }
            }
            memset(&s_devices[target_idx], 0, sizeof(discovered_device_t));
            memcpy(s_devices[target_idx].mac_or_id, norm_id, 8);
            s_devices[target_idx].protocol = protocol;
        }

        discovered_device_t& dev = s_devices[target_idx];
        dev.rssi = rssi;
        dev.channel = channel;
        dev.ble_addr_type = ble_addr_type;
        dev.ble_connectable = ble_connectable;
        dev.last_seen_ms = millis();

        if (name && name[0] != '\0') {
            strncpy(dev.name, name, sizeof(dev.name) - 1);
            dev.name[sizeof(dev.name) - 1] = '\0';
        } else if (dev.name[0] == '\0') {
            strncpy(dev.name, "<Hidden/Unnamed>", sizeof(dev.name) - 1);
        }

        if (details && details[0] != '\0') {
            strncpy(dev.details, details, sizeof(dev.details) - 1);
            dev.details[sizeof(dev.details) - 1] = '\0';
        }

        log_copy = dev;
        s_table_revision++;
        xSemaphoreGive(data_table_mutex);
    }

    if (is_new) {
        log_buffer_push(protocol, "%s (%ddBm)", log_copy.name, (int)log_copy.rssi);
        sd_logger_enqueue(log_copy);
    }

    return is_new;
}

bool device_table_lookup_ble(const uint8_t mac[6],
                             uint8_t* out_addr_type,
                             bool* out_connectable,
                             char* out_name,
                             size_t name_len) {
    if (!mac) return false;
    uint8_t norm_id[8] = {0};
    memcpy(norm_id, mac, 6);

    bool found = false;
    if (data_table_mutex && xSemaphoreTake(data_table_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (size_t i = 0; i < s_device_count; ++i) {
            if ((s_devices[i].protocol == PROTO_BLE || s_devices[i].protocol == PROTO_MATTER) &&
                memcmp(s_devices[i].mac_or_id, norm_id, 6) == 0) {
                if (out_addr_type) *out_addr_type = s_devices[i].ble_addr_type;
                if (out_connectable) *out_connectable = s_devices[i].ble_connectable;
                if (out_name && name_len > 0) {
                    strncpy(out_name, s_devices[i].name, name_len - 1);
                    out_name[name_len - 1] = '\0';
                }
                found = true;
                break;
            }
        }
        xSemaphoreGive(data_table_mutex);
    }
    return found;
}

bool parse_mac_str(const char* mac_str, uint8_t out_mac[6]) {
    if (!mac_str || !out_mac) return false;
    unsigned int b[6] = {0};
    int parsed = sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                        &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    if (parsed != 6) {
        parsed = sscanf(mac_str, "%02x-%02x-%02x-%02x-%02x-%02x",
                        &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    }
    if (parsed != 6) return false;
    for (int i = 0; i < 6; ++i) {
        out_mac[i] = (uint8_t)(b[i] & 0xFF);
    }
    return true;
}

protocol_counts_t device_table_get_counts() {
    protocol_counts_t counts = {0, 0, 0, 0, 0};
    if (data_table_mutex && xSemaphoreTake(data_table_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (size_t i = 0; i < s_device_count; ++i) {
            switch (s_devices[i].protocol) {
                case PROTO_WIFI:   counts.wifi_count++;   break;
                case PROTO_BLE:    counts.ble_count++;    break;
                case PROTO_ZIGBEE: counts.zigbee_count++; break;
                case PROTO_MATTER: counts.matter_count++; break;
                default: break;
            }
        }
        counts.total_count = (uint16_t)s_device_count;
        xSemaphoreGive(data_table_mutex);
    }
    return counts;
}

size_t device_table_snapshot(discovered_device_t* out_array, size_t max_items) {
    if (!out_array || max_items == 0) return 0;
    size_t copied = 0;
    if (data_table_mutex && xSemaphoreTake(data_table_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        copied = (s_device_count < max_items) ? s_device_count : max_items;
        memcpy(out_array, s_devices, copied * sizeof(discovered_device_t));
        xSemaphoreGive(data_table_mutex);
    }
    return copied;
}

uint32_t device_table_get_revision() {
    return s_table_revision;
}

