#pragma once

#include <Arduino.h>

// Default Wi-Fi SoftAP & HTTP Authentication Configuration (FR-1, FR-11)
#define DEFAULT_AP_SSID       "chip-scanner"
#define DEFAULT_AP_CHANNEL    1
#define DEFAULT_HTTP_USER     "admin"
#define DEFAULT_HTTP_PASS     "pw4ggyy!"

// Brute-force lockout thresholds (NFR-7)
#define AUTH_MAX_FAILURES     5
#define AUTH_LOCKOUT_MS       5000

typedef struct {
    bool auto_scan;
    uint16_t interval_s;
    bool lcd_backlight;
    char http_user[24];
    char http_pass[32];
} scanner_config_t;

extern scanner_config_t g_config;

void auth_config_init();
void auth_config_save();
bool auth_verify_credentials(const char* username, const char* password, const char* client_ip);
bool auth_is_locked_out();

