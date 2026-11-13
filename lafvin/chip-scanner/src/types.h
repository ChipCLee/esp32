#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================================
// Hardware Pin Definitions (LAFVIN ESP32-C6-1.47LCD / Model LB008)
// ============================================================================
#define PIN_SPI_MOSI        6
#define PIN_SPI_SCK         7
#define PIN_SPI_MISO        5

#define PIN_LCD_CS          14
#define PIN_LCD_DC          15
#define PIN_LCD_RST         21
#define PIN_LCD_BL          22

#define PIN_SD_CS           4
#define PIN_WS2812_RGB      8
#define PIN_USER_BTN        9

// LCD Geometry & Calibration
#define LCD_WIDTH           172
#define LCD_HEIGHT          320
#define LCD_COL_OFFSET      34

// ============================================================================
// Protocol & State Enumerations
// ============================================================================
typedef enum : uint8_t {
    PROTO_WIFI   = 0,
    PROTO_BLE    = 1,
    PROTO_ZIGBEE = 2,
    PROTO_MATTER = 3,
    PROTO_SYSTEM = 4
} rf_protocol_t;

typedef enum : uint8_t {
    SCAN_STATE_IDLE     = 0,
    SCAN_STATE_WIFI     = 1,
    SCAN_STATE_BLE      = 2,
    SCAN_STATE_ZIGBEE   = 3,
    SCAN_STATE_ALL      = 4,
    SCAN_STATE_BLE_GATT = 5
} scan_state_t;

// ============================================================================
// Core In-Memory Data Models (Section 3.3 & ble.md Section 3.2)
// ============================================================================
typedef struct {
    uint8_t mac_or_id[8];     // 6-byte MAC Address or 8-byte IEEE 802.15.4 Extended PAN ID
    char name[33];            // SSID, BLE Advertised Name, or "Zigbee PAN 0xXXXX"
    int8_t rssi;              // Received Signal Strength Indicator (dBm)
    uint8_t channel;          // 1-13 (Wi-Fi), 37-39 (BLE), 11-26 (802.15.4)
    rf_protocol_t protocol;   // Protocol category
    uint8_t ble_addr_type;    // BLE_ADDR_PUBLIC (0), BLE_ADDR_RANDOM (1), etc.
    bool ble_connectable;     // True if BLE advertisement is connectable
    char details[44];         // e.g., "WPA3 | Wi-Fi 6", "AirTag", "LQI:195", "VID:0x1234"
    uint32_t last_seen_ms;    // millis() timestamp for aging/eviction
} discovered_device_t;        // 92 bytes per record

typedef struct {
    uint32_t timestamp_s;     // Unix epoch (if browser-synced) or boot uptime seconds
    bool is_wall_clock;       // True if timestamp_s is synced Unix epoch
    rf_protocol_t protocol;   // Associated protocol badge
    char message[38];         // Event summary for LCD & Web Log table
} lcd_log_entry_t;

#define MAX_DISCOVERED_DEVICES 100
#define LCD_LOG_HISTORY_SIZE   16

// ============================================================================
// Global Synchronization Primitives
// ============================================================================
extern SemaphoreHandle_t data_table_mutex;
extern SemaphoreHandle_t spi_bus_mutex;

// Helper to convert protocol enum to short string
inline const char* protocol_to_str(rf_protocol_t proto) {
    switch (proto) {
        case PROTO_WIFI:   return "WIFI";
        case PROTO_BLE:    return "BLE";
        case PROTO_ZIGBEE: return "15.4";
        case PROTO_MATTER: return "MAT";
        case PROTO_SYSTEM: return "AP";
        default:           return "UNK";
    }
}

