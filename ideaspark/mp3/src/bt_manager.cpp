#include "bt_manager.h"
#include "mp3_player.h"
#include <BluetoothA2DPSource.h>
#include <Preferences.h>

static BluetoothA2DPSource s_a2dp_source;
static Preferences s_prefs;

static bt_device_info_t s_bt_devices[MAX_BT_DEVICES];
static size_t s_bt_device_count = 0;

static volatile bool s_bt_connected = false;
static volatile bool s_bt_scanning = false;
static bool s_bt_started = false;
static uint32_t s_scan_start_ms = 0;

static uint8_t s_target_mac[6] = {0};
static bool    s_has_target_mac = false;
static char    s_target_name[36] = {0};
static char    s_connected_mac_str[18] = {0};
static char    s_connected_name[36] = {0};

static bool parse_bt_mac(const char* str, uint8_t out[6]) {
    if (!str || !out) return false;
    unsigned int b[6] = {0};
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) out[i] = (uint8_t)(b[i] & 0xFF);
    return true;
}

static void format_bt_mac(const uint8_t mac[6], char* out, size_t len) {
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Audio data callback called by Bluedroid A2DP Source on Core 0
static int32_t a2dp_get_data_frames(Frame* frame, int32_t frame_count) {
    return mp3_player_read_a2dp_frames((uint8_t*)frame, frame_count);
}

static void a2dp_conn_state_cb(esp_a2d_connection_state_t state, void* ptr) {
    (void)ptr;
    if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        s_bt_connected = true;
        s_bt_scanning = false;
        digitalWrite(PIN_STATUS_LED, HIGH);
        Serial.printf("[BT] Connected to A2DP Sink: %s (%s)\n", s_connected_name, s_connected_mac_str);
    } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        s_bt_connected = false;
        digitalWrite(PIN_STATUS_LED, LOW);
        Serial.println("[BT] Disconnected from A2DP Sink");
    }
}

// Built-in ESP32-A2DP discovery callback (avoids overwriting esp_bt_gap_register_callback)
static bool on_bt_ssid_discovered(const char* ssid, esp_bd_addr_t address, int rssi) {
    if (address == nullptr) return false;

    char mac_str[18];
    format_bt_mac(address, mac_str, sizeof(mac_str));
    const char* safe_name = (ssid && ssid[0] != '\0') ? ssid : "<Unnamed BT Device>";

    if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        int found = -1;
        for (size_t i = 0; i < s_bt_device_count; ++i) {
            if (memcmp(s_bt_devices[i].mac, address, 6) == 0) {
                found = (int)i;
                break;
            }
        }
        size_t idx = (found >= 0) ? (size_t)found : s_bt_device_count;
        if (idx < MAX_BT_DEVICES) {
            if (found < 0) s_bt_device_count++;
            memcpy(s_bt_devices[idx].mac, address, 6);
            strncpy(s_bt_devices[idx].mac_str, mac_str, sizeof(s_bt_devices[idx].mac_str) - 1);
            strncpy(s_bt_devices[idx].name, safe_name, sizeof(s_bt_devices[idx].name) - 1);
            s_bt_devices[idx].rssi = rssi;
        }
        xSemaphoreGive(state_mutex);
    }

    // In scan-only mode, record devices and return false so discovery continues
    if (s_bt_scanning) {
        return false;
    }

    // In connect mode, match target MAC or target name
    bool match = false;
    if (s_has_target_mac && memcmp(address, s_target_mac, 6) == 0) {
        match = true;
    } else if (s_target_name[0] != '\0' && ssid && strcasecmp(ssid, s_target_name) == 0) {
        match = true;
    }

    if (match) {
        strncpy(s_connected_name, safe_name, sizeof(s_connected_name) - 1);
        strncpy(s_connected_mac_str, mac_str, sizeof(s_connected_mac_str) - 1);
        return true;
    }
    return false;
}

static void bt_ensure_started() {
    if (s_bt_started) return;
    s_a2dp_source.set_auto_reconnect(false);
    s_a2dp_source.set_ssp_enabled(true);
    s_a2dp_source.set_ssid_callback(on_bt_ssid_discovered);
    s_a2dp_source.set_on_connection_state_changed(a2dp_conn_state_cb);
    s_a2dp_source.set_data_callback_in_frames(a2dp_get_data_frames);
    s_a2dp_source.set_local_name("Ideaspark-MP3");

    std::vector<const char*> empty_names;
    s_a2dp_source.start(empty_names);
    s_bt_started = true;
}

void bt_manager_init() {
    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);

    // Defer starting Bluedroid A2DP discovery until scan/connect/playback so
    // Classic BT Inquiry doesn't starve the 2.4 GHz Wi-Fi SoftAP at boot.
    s_prefs.begin("bt_cfg", true);
    String saved_name = s_prefs.getString("name", "");
    String saved_mac  = s_prefs.getString("mac", "");
    s_prefs.end();

    if (saved_mac.length() == 17) {
        Serial.printf("[BT] Auto-connecting saved speaker: %s (%s)\n", saved_name.c_str(), saved_mac.c_str());
        bt_manager_connect(saved_name.c_str(), saved_mac.c_str());
    }
}

void bt_manager_start_scan() {
    bt_ensure_started();
    if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_bt_device_count = 0;
        xSemaphoreGive(state_mutex);
    }
    s_bt_scanning = true;
    s_scan_start_ms = millis();
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 7 /* ~9s */, 0);
}

bool bt_manager_is_scanning() {
    if (s_bt_scanning && (millis() - s_scan_start_ms > 10000UL)) {
        s_bt_scanning = false;
    }
    return s_bt_scanning;
}

size_t bt_manager_get_devices(bt_device_info_t* out_devs, size_t max_devs) {
    if (!out_devs || max_devs == 0) return 0;
    size_t cnt = 0;
    if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cnt = (s_bt_device_count < max_devs) ? s_bt_device_count : max_devs;
        memcpy(out_devs, s_bt_devices, cnt * sizeof(bt_device_info_t));
        xSemaphoreGive(state_mutex);
    }
    return cnt;
}

bool bt_manager_connect(const char* name, const char* mac_str) {
    bt_ensure_started();
    s_bt_scanning = false;
    esp_bt_gap_cancel_discovery();

    if (s_a2dp_source.is_connected()) {
        s_a2dp_source.disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    memset(s_target_mac, 0, sizeof(s_target_mac));
    s_has_target_mac = parse_bt_mac(mac_str, s_target_mac);

    if (name && name[0] != '\0') {
        strncpy(s_target_name, name, sizeof(s_target_name) - 1);
        s_target_name[sizeof(s_target_name) - 1] = '\0';
        strncpy(s_connected_name, name, sizeof(s_connected_name) - 1);
    } else {
        s_target_name[0] = '\0';
        strncpy(s_connected_name, mac_str ? mac_str : "BT Speaker", sizeof(s_connected_name) - 1);
    }

    if (mac_str && mac_str[0] != '\0') {
        strncpy(s_connected_mac_str, mac_str, sizeof(s_connected_mac_str) - 1);
    }

    // Persist speaker to NVS
    s_prefs.begin("bt_cfg", false);
    s_prefs.putString("name", s_connected_name);
    s_prefs.putString("mac", s_connected_mac_str);
    s_prefs.end();

    if (s_has_target_mac) {
        return s_a2dp_source.connect_to(s_target_mac);
    }
    // Fallback: start discovery so on_bt_ssid_discovered matches by name
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 7, 0);
    return true;
}

void bt_manager_disconnect() {
    s_bt_scanning = false;
    esp_bt_gap_cancel_discovery();
    if (s_a2dp_source.is_connected()) {
        s_a2dp_source.disconnect();
    }
    s_bt_connected = false;
    digitalWrite(PIN_STATUS_LED, LOW);

    s_prefs.begin("bt_cfg", false);
    s_prefs.clear();
    s_prefs.end();
}

bool bt_manager_is_connected() {
    return s_a2dp_source.is_connected();
}

void bt_manager_get_status(bool* out_connected,
                           bool* out_scanning,
                           char* out_name,
                           size_t name_len,
                           char* out_mac,
                           size_t mac_len) {
    if (out_connected) *out_connected = s_a2dp_source.is_connected();
    if (out_scanning)  *out_scanning  = bt_manager_is_scanning();
    if (out_name && name_len > 0) {
        strncpy(out_name, s_connected_name, name_len - 1);
        out_name[name_len - 1] = '\0';
    }
    if (out_mac && mac_len > 0) {
        strncpy(out_mac, s_connected_mac_str, mac_len - 1);
        out_mac[mac_len - 1] = '\0';
    }
}
