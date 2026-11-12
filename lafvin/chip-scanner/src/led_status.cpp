#include "led_status.h"
#include <Adafruit_NeoPixel.h>

static Adafruit_NeoPixel s_pixel(1, PIN_WS2812_RGB, NEO_GRB + NEO_KHZ800);
static volatile led_state_t s_current_state = LED_STATE_IDLE_AP;
static uint8_t s_breath_phase = 0;

void led_status_init() {
    s_pixel.begin();
    s_pixel.setBrightness(35);
    s_pixel.setPixelColor(0, s_pixel.Color(0, 200, 0));
    s_pixel.show();
}

void led_status_set(led_state_t state) {
    s_current_state = state;
    led_status_tick();
}

led_state_t led_status_get() {
    return s_current_state;
}

void led_status_tick() {
    s_breath_phase += 8;
    // Simple triangle wave 60..220 for subtle breathing indicator
    uint8_t wave = (s_breath_phase < 128) ? (60 + s_breath_phase) : (60 + (255 - s_breath_phase));

    uint32_t color = 0;
    switch (s_current_state) {
        case LED_STATE_IDLE_AP:
            // Green (FR-9: Wi-Fi AP serving)
            color = s_pixel.Color(0, wave, 0);
            break;
        case LED_STATE_SCAN_WIFI:
            // Cyan-Green (Wi-Fi scan)
            color = s_pixel.Color(0, 220, 140);
            break;
        case LED_STATE_SCAN_BLE:
            // Blue (FR-9: BLE scan)
            color = s_pixel.Color(0, 60, 255);
            break;
        case LED_STATE_SCAN_154:
            // Purple (FR-9: 802.15.4 / Zigbee scan)
            color = s_pixel.Color(170, 0, 255);
            break;
        case LED_STATE_ERROR:
            // Red (FR-9: Error / Auth lockout)
            color = s_pixel.Color(255, 0, 0);
            break;
    }

    s_pixel.setPixelColor(0, color);
    s_pixel.show();
}

