#include "scanner_wifi.h"
#include "auth_config.h"
#include "device_table.h"
#include "log_buffer.h"
#include <WiFi.h>
#include <esp_wifi.h>

static uint8_t s_last_client_count = 0;

static const char* auth_mode_to_str(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
        default:                        return "SECURED";
    }
}

void scanner_wifi_init() {
    WiFi.mode(WIFI_AP_STA);

    // Configure static SoftAP IP 192.168.4.1 (FR-1)
    IPAddress local_ip(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(local_ip, gateway, subnet);

    // Start SoftAP "chip-scanner" on channel 1
    WiFi.softAP(DEFAULT_AP_SSID, nullptr, DEFAULT_AP_CHANNEL, 0, 4);

    // Enable Wi-Fi 6 (802.11ax HE) protocol support on AP & STA interfaces
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX);
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX);

    log_buffer_push(PROTO_SYSTEM, "AP '%s' @ 192.168.4.1", DEFAULT_AP_SSID);
}

uint8_t scanner_wifi_get_connected_users() {
    return WiFi.softAPgetStationNum();
}

void scanner_wifi_check_client_events() {
    uint8_t current = WiFi.softAPgetStationNum();
    if (current != s_last_client_count) {
        if (current > s_last_client_count) {
            log_buffer_push(PROTO_SYSTEM, "User joined AP (Users: %u)", (unsigned)current);
        } else {
            log_buffer_push(PROTO_SYSTEM, "User left AP (Users: %u)", (unsigned)current);
        }
        s_last_client_count = current;
    }
}

void scanner_wifi_scan_step(uint8_t start_channel, uint8_t num_channels) {
    for (uint8_t c = 0; c < num_channels; ++c) {
        uint8_t ch = start_channel + c;
        if (ch < 1 || ch > 13) continue;

        // Scan single channel for 40ms max dwell time (Section 3.2 micro-window <= 90ms)
        int16_t n = WiFi.scanNetworks(false /* async */, true /* show_hidden */, false /* passive */, 40 /* max_ms_per_chan */, ch);
        if (n > 0) {
            for (int16_t i = 0; i < n; ++i) {
                uint8_t* bssid = WiFi.BSSID(i);
                if (!bssid) continue;

                String ssid = WiFi.SSID(i);
                int32_t rssi = WiFi.RSSI(i);
                int32_t chan = WiFi.channel(i);
                wifi_auth_mode_t auth = WiFi.encryptionType(i);

                // Check 802.11ax / Wi-Fi 6 capability from AP record if available
                wifi_ap_record_t* ap_rec = (wifi_ap_record_t*)WiFi.getScanInfoByIndex(i);
                const char* gen_str = "Wi-Fi 4";
                if (ap_rec) {
                    if (ap_rec->phy_11ax) {
                        gen_str = "Wi-Fi 6";
                    } else if (ap_rec->phy_11n) {
                        gen_str = "Wi-Fi 4";
                    } else {
                        gen_str = "802.11b/g";
                    }
                }

                char details[44];
                snprintf(details, sizeof(details), "%s | %s", auth_mode_to_str(auth), gen_str);

                device_table_upsert(
                    PROTO_WIFI,
                    bssid,
                    6,
                    ssid.length() > 0 ? ssid.c_str() : "<Hidden SSID>",
                    (int8_t)rssi,
                    (uint8_t)chan,
                    details
                );
            }
        }
        WiFi.scanDelete();
    }
}

void scanner_wifi_full_sweep() {
    // Channel-interleaved scan across channels 1..13 (2 channels per burst + 100ms SoftAP recovery)
    for (uint8_t ch = 1; ch <= 13; ch += 2) {
        uint8_t count = (ch == 13) ? 1 : 2;
        scanner_wifi_scan_step(ch, count);
        // 100ms Home-Channel Recovery Window so connected web browsers never drop DTIM beacons
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

