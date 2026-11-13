#include "auth_config.h"
#include "log_buffer.h"
#include "led_status.h"
#include <Preferences.h>

scanner_config_t g_config;
static Preferences s_prefs;
static uint8_t s_consecutive_failures = 0;
static uint32_t s_lockout_until_ms = 0;

// Constant-time string comparison to prevent timing side-channel attacks (NFR-7)
static bool constant_time_equals(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    volatile uint8_t diff = (len_a != len_b) ? 1 : 0;
    size_t max_len = (len_a > len_b) ? len_a : len_b;
    for (size_t i = 0; i < max_len; ++i) {
        uint8_t ca = (i < len_a) ? (uint8_t)a[i] : 0;
        uint8_t cb = (i < len_b) ? (uint8_t)b[i] : 0;
        diff |= (ca ^ cb);
    }
    return (diff == 0);
}

void auth_config_init() {
    g_config.auto_scan = true;
    g_config.interval_s = 15;
    g_config.lcd_backlight = true;
    strncpy(g_config.ap_pass, DEFAULT_AP_PASS, sizeof(g_config.ap_pass) - 1);
    g_config.ap_pass[sizeof(g_config.ap_pass) - 1] = '\0';
    strncpy(g_config.http_user, DEFAULT_HTTP_USER, sizeof(g_config.http_user) - 1);
    g_config.http_user[sizeof(g_config.http_user) - 1] = '\0';
    strncpy(g_config.http_pass, DEFAULT_HTTP_PASS, sizeof(g_config.http_pass) - 1);
    g_config.http_pass[sizeof(g_config.http_pass) - 1] = '\0';

    if (s_prefs.begin("chip_scan", true)) {
        g_config.auto_scan = s_prefs.getBool("auto_scan", true);
        g_config.interval_s = s_prefs.getUShort("interval_s", 15);
        g_config.lcd_backlight = s_prefs.getBool("lcd_bl", true);
        String saved_ap_pass = s_prefs.getString("ap_pass", DEFAULT_AP_PASS);
        if (saved_ap_pass.length() >= 8 && saved_ap_pass.length() < sizeof(g_config.ap_pass)) {
            strncpy(g_config.ap_pass, saved_ap_pass.c_str(), sizeof(g_config.ap_pass) - 1);
            g_config.ap_pass[sizeof(g_config.ap_pass) - 1] = '\0';
        }
        String saved_http_pass = s_prefs.getString("http_pass", DEFAULT_HTTP_PASS);
        if (saved_http_pass.length() >= 4 && saved_http_pass.length() < sizeof(g_config.http_pass)) {
            strncpy(g_config.http_pass, saved_http_pass.c_str(), sizeof(g_config.http_pass) - 1);
            g_config.http_pass[sizeof(g_config.http_pass) - 1] = '\0';
        }
        s_prefs.end();
    }
}

void auth_config_save() {
    if (s_prefs.begin("chip_scan", false)) {
        s_prefs.putBool("auto_scan", g_config.auto_scan);
        s_prefs.putUShort("interval_s", g_config.interval_s);
        s_prefs.putBool("lcd_bl", g_config.lcd_backlight);
        s_prefs.putString("ap_pass", g_config.ap_pass);
        s_prefs.putString("http_pass", g_config.http_pass);
        s_prefs.end();
    }
}

bool auth_is_locked_out() {
    if (s_lockout_until_ms == 0) return false;
    uint32_t now = millis();
    if ((int32_t)(now - s_lockout_until_ms) < 0) {
        return true;
    }
    s_lockout_until_ms = 0;
    s_consecutive_failures = 0;
    return false;
}

bool auth_verify_credentials(const char* username, const char* password, const char* client_ip) {
    if (auth_is_locked_out()) {
        return false;
    }

    bool user_ok = constant_time_equals(username, g_config.http_user);
    bool pass_ok = constant_time_equals(password, g_config.http_pass);

    if (user_ok && pass_ok) {
        s_consecutive_failures = 0;
        s_lockout_until_ms = 0;
        return true;
    }

    s_consecutive_failures++;
    char log_msg[38];
    snprintf(log_msg, sizeof(log_msg), "Auth failed: %s", client_ip ? client_ip : "client");
    log_buffer_push(PROTO_SYSTEM, log_msg);

    if (s_consecutive_failures >= AUTH_MAX_FAILURES) {
        s_lockout_until_ms = millis() + AUTH_LOCKOUT_MS;
        led_status_set(LED_STATE_ERROR);
    }

    return false;
}

