#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================================
// Hardware Pin Definitions — Ideaspark ESP32 OLED (ESP32-WROOM-32)
// ============================================================================
#define PIN_OLED_SDA        21
#define PIN_OLED_SCL        22
#define OLED_I2C_ADDR       0x3C
#define OLED_WIDTH          128
#define OLED_HEIGHT         64

#define PIN_BOOT_BTN        0     // Right-side BOOT button (Active LOW after boot)
#define PIN_STATUS_LED      2     // Onboard blue status LED

// ============================================================================
// Wi-Fi SoftAP Configuration
// ============================================================================
#define AP_SSID             "ideaspark-mp3"
#define AP_PASS             "pw4ggyy!"
#define AP_CHANNEL          1

// ============================================================================
// Storage & Limits
// ============================================================================
#define MAX_TRACKS          32
#define MAX_BT_DEVICES      16
#define LITTLEFS_MAX_PATH   31    // ESP32 LittleFS path limit (including '/')

typedef enum : uint8_t {
    PLAYER_STOPPED = 0,
    PLAYER_PLAYING = 1,
    PLAYER_PAUSED  = 2
} player_state_t;

typedef struct {
    char path[32];          // e.g. "/midnight.mp3" (<= 31 chars)
    char title[36];         // ID3 Title or fallback filename
    char artist[36];        // ID3 Artist or "Local MP3 Track"
    uint32_t size_bytes;    // File size in bytes
} mp3_track_t;

typedef struct {
    char name[36];
    uint8_t mac[6];
    char mac_str[18];
    int rssi;
} bt_device_info_t;

// ============================================================================
// Global Synchronization Primitives
// ============================================================================
extern SemaphoreHandle_t fs_mutex;
extern SemaphoreHandle_t state_mutex;
