#pragma once

#include "types.h"

typedef struct {
    uint16_t wifi_count;
    uint16_t ble_count;
    uint16_t zigbee_count;
    uint16_t matter_count;
    uint16_t total_count;
} protocol_counts_t;

void device_table_init();
void device_table_clear();

// Insert or update a discovered RF entity. Returns true if it was newly added.
bool device_table_upsert(rf_protocol_t protocol,
                         const uint8_t* mac_or_id,
                         size_t id_len,
                         const char* name,
                         int8_t rssi,
                         uint8_t channel,
                         const char* details);

protocol_counts_t device_table_get_counts();
size_t device_table_snapshot(discovered_device_t* out_array, size_t max_items);
uint32_t device_table_get_revision();

// Format 6-byte MAC or 8-byte PAN/ExtID into hex string
void format_mac_or_id(rf_protocol_t proto, const uint8_t* id, char* out_str, size_t out_len);

