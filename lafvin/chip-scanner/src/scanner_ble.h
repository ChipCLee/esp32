#pragma once

#include "types.h"

#define MAX_GATT_SERVICES          12
#define MAX_GATT_TOTAL_CHARS       48
#define MAX_GATT_READ_LEN          128

// Standard BLE GATT Characteristic Property Bitmask (Bluetooth Core Spec Vol 3, Part G, 3.3.1.1)
#define BLE_PROP_BROADCAST         0x01
#define BLE_PROP_READ              0x02
#define BLE_PROP_WRITE_NR          0x04
#define BLE_PROP_WRITE             0x08
#define BLE_PROP_NOTIFY            0x10
#define BLE_PROP_INDICATE          0x20
#define BLE_PROP_AUTH_SIGN         0x40
#define BLE_PROP_EXTENDED          0x80

typedef struct {
    char uuid[37];            // "0x2a19" (16-bit) or full 128-bit UUID string
    uint16_t handle;          // GATT Characteristic Value Attribute Handle
    uint8_t svc_index;        // Index into parent services[] array
    uint8_t properties;       // Bitmask of BLE_PROP_*
} ble_gatt_char_info_t;       // 42 bytes (48 items = 2,016 bytes)

typedef struct {
    char uuid[37];            // "0x180f" (16-bit) or full 128-bit UUID string
    uint16_t start_handle;    // Service start attribute handle
    uint16_t end_handle;      // Service end attribute handle
    uint8_t char_start_idx;   // Starting index in the flat chars[] array
    uint8_t char_count;       // Number of characteristics in this service
} ble_gatt_service_info_t;    // 44 bytes (12 items = 528 bytes)

typedef struct {
    bool success;
    bool connected;
    uint8_t mac[6];
    uint8_t addr_type;
    char device_name[33];
    int8_t rssi;
    uint16_t mtu;
    char error_msg[64];
    uint8_t service_count;
    uint8_t total_char_count;
    ble_gatt_service_info_t services[MAX_GATT_SERVICES];
    ble_gatt_char_info_t chars[MAX_GATT_TOTAL_CHARS];
} ble_gatt_discovery_result_t; // ~2.59 KB in static .bss

typedef struct {
    bool success;
    uint8_t mac[6];
    char service_uuid[37];
    char char_uuid[37];
    uint16_t handle;
    uint8_t properties;
    int8_t rssi;
    uint16_t value_len;
    uint8_t raw_bytes[MAX_GATT_READ_LEN];
    char hex_str[MAX_GATT_READ_LEN * 3 + 1];
    char utf8_str[MAX_GATT_READ_LEN + 1];
    char decoded_str[96];     // Human-friendly decoded representation (e.g. "88 %", "23.45 °C")
    char error_msg[64];
} ble_gatt_read_result_t;     // ~0.88 KB in static .bss

void scanner_ble_init();
// Runs a coexistence-safe active BLE 5.3 & Matter discovery window
void scanner_ble_scan_window(uint32_t duration_ms);
void scanner_ble_stop_scan();

// BLE GATT Central Operations (ble.md Section 3.4)
const ble_gatt_discovery_result_t* scanner_ble_discover_gatt(const uint8_t mac[6], int addr_type_override);
const ble_gatt_read_result_t* scanner_ble_read_characteristic(const uint8_t mac[6],
                                                              int addr_type_override,
                                                              const char* service_uuid,
                                                              const char* char_uuid,
                                                              uint16_t handle);
void scanner_ble_disconnect_gatt();
void scanner_ble_check_gatt_idle();
bool scanner_ble_is_gatt_connected();
void scanner_ble_get_gatt_status(bool* out_connected,
                                 uint8_t out_mac[6],
                                 char* out_name,
                                 size_t name_len,
                                 int8_t* out_rssi,
                                 uint16_t* out_mtu,
                                 uint32_t* out_idle_s);
