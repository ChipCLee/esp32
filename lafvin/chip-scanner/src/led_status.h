#pragma once

#include "types.h"

typedef enum : uint8_t {
    LED_STATE_IDLE_AP = 0, // Green: Wi-Fi AP serving
    LED_STATE_SCAN_WIFI,   // Cyan-Green: Wi-Fi scan active
    LED_STATE_SCAN_BLE,    // Blue: BLE 5.3 scan active
    LED_STATE_SCAN_154,    // Purple: IEEE 802.15.4 / Zigbee scan active
    LED_STATE_ERROR        // Red: Error / Auth lockout
} led_state_t;

void led_status_init();
void led_status_set(led_state_t state);
led_state_t led_status_get();
void led_status_tick();

