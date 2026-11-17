#include "bt_manager.h"
#include "mp3_player.h"
#include "web_server.h"
#include <BluetoothA2DPSource.h>
#include <Preferences.h>
#include <esp_wifi.h>

static BluetoothA2DPSource s_a2dp_source;
static Preferences s_prefs;

static bt_device_info_t s_bt_devices[MAX_BT_DEVICES];
static size_t s_bt_device_count = 0;

static volatile bool s_bt_connected = false;
static volatile bool s_bt_scanning = false;
static volatile bool s_auto_pair_mode = false;
static volatile bool s_auto_play_on_connect = false;
static volatile bool s_pending_autoplay = false;
static volatile bool s_fallback_scan_pending = false;
static volatile bool s_pending_fallback_scan = false;
static volatile bool s_was_connected = false;
static volatile bool s_pending_reconnect = false;
static bool s_bt_started = false;
static uint32_t s_scan_start_ms = 0;
static uint32_t s_scan_timeout_ms = 10000UL;

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
        s_was_connected = true;
        s_bt_scanning = false;
        s_auto_pair_mode = false;
        s_fallback_scan_pending = false;
        s_pending_reconnect = false;
        digitalWrite(PIN_STATUS_LED, HIGH);
        Serial.printf("[BT] Connected to A2DP Sink: %s (%s)\n", s_connected_name, s_connected_mac_str);
        if (s_auto_play_on_connect) {
            s_auto_play_on_connect = false;
            s_pending_autoplay = true;
        }
    } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        s_bt_connected = false;
        digitalWrite(PIN_STATUS_LED, LOW);
        Serial.println("[BT] Disconnected from A2DP Sink");
        if (s_fallback_scan_pending) {
            s_fallback_scan_pending = false;
            s_pending_fallback_scan = true;
        } else if (s_was_connected && app_get_mode() == APP_MODE_BLUETOOTH && s_has_target_mac) {
            s_was_connected = false;
            s_auto_play_on_connect = true;
            s_pending_reconnect = true;
        }
    }
}

// Built-in ESP32-A2DP discovery callback (avoids overwriting esp_bt_gap_register_callback)
static bool on_bt_ssid_discovered(const char* ssid, esp_bd_addr_t address, int rssi) {
    if (address == nullptr) return false;

    char mac_str[18];
    format_bt_mac(address, mac_str, sizeof(mac_str));
    const char* safe_name = (ssid && ssid[0] != '\0') ? ssid : "<Unnamed BT Device>";
    Serial.printf("[BT] Discovered: %s (%s) rssi=%d\n", safe_name, mac_str, rssi);

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

    // In Bluetooth auto-pair mode (no saved speaker), pair with the first named speaker found
    if (s_auto_pair_mode && ssid && ssid[0] != '\0') {
        s_auto_pair_mode = false;
        s_bt_scanning = false;
        memcpy(s_target_mac, address, 6);
        s_has_target_mac = true;
        strncpy(s_target_name, safe_name, sizeof(s_target_name) - 1);
        strncpy(s_connected_name, safe_name, sizeof(s_connected_name) - 1);
        strncpy(s_connected_mac_str, mac_str, sizeof(s_connected_mac_str) - 1);

        s_prefs.begin("bt_cfg", false);
        s_prefs.putString("name", s_connected_name);
        s_prefs.putString("mac", s_connected_mac_str);
        s_prefs.end();

        Serial.printf("[BT] Auto-pairing discovered speaker: %s (%s)\n", s_connected_name, s_connected_mac_str);
        return true;
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

static void on_bt_discovery_mode(esp_bt_gap_discovery_state_t state) {
    Serial.printf("[BT] Discovery state: %s\n",
                  state == ESP_BT_GAP_DISCOVERY_STARTED ? "STARTED" : "STOPPED");
    if (state == ESP_BT_GAP_DISCOVERY_STOPPED && (millis() - s_scan_start_ms > 2000UL)) {
        s_bt_scanning = false;
    }
}

static void bt_ensure_started(bool auto_connect_target = false) {
    if (s_bt_started) return;
    Serial.printf("[BT] Starting Bluedroid A2DP (free heap=%u, max block=%u)...\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    // Enable Wi-Fi modem sleep for Wi-Fi/BT coexistence when SoftAP is active
    if (web_server_is_active()) {
        esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        Serial.printf("[BT] esp_wifi_set_ps(MIN_MODEM) -> 0x%x\n", (unsigned)ps_err);
    }
    if (auto_connect_target && s_has_target_mac) {
        s_a2dp_source.set_auto_reconnect(s_target_mac, 1);
    } else {
        s_a2dp_source.set_auto_reconnect(false);
    }
    s_a2dp_source.set_ssp_enabled(true);
    s_a2dp_source.set_valid_cod_service(ESP_BT_COD_SRVC_RENDERING | ESP_BT_COD_SRVC_AUDIO);
    s_a2dp_source.set_ssid_callback(on_bt_ssid_discovered);
    s_a2dp_source.set_discovery_mode_callback(on_bt_discovery_mode);
    s_a2dp_source.set_on_connection_state_changed(a2dp_conn_state_cb);
    s_a2dp_source.set_data_callback_in_frames(a2dp_get_data_frames);
    s_a2dp_source.set_local_name("Ideaspark-MP3");

    std::vector<const char*> empty_names;
    s_a2dp_source.start(empty_names);
    esp_bredr_tx_power_set(ESP_PWR_LVL_P3, ESP_PWR_LVL_P9);
    s_bt_started = true;
    Serial.printf("[BT] Started: ctrl=%d bluedroid=%d free_heap=%u\n",
                  (int)esp_bt_controller_get_status(),
                  (int)esp_bluedroid_get_status(),
                  (unsigned)ESP.getFreeHeap());
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
        s_has_target_mac = parse_bt_mac(saved_mac.c_str(), s_target_mac);
        strncpy(s_connected_mac_str, saved_mac.c_str(), sizeof(s_connected_mac_str) - 1);
        strncpy(s_connected_name, saved_name.c_str(), sizeof(s_connected_name) - 1);
        strncpy(s_target_name, saved_name.c_str(), sizeof(s_target_name) - 1);
        Serial.printf("[BT] Saved speaker in NVS: %s (%s)\n", s_connected_name, s_connected_mac_str);
    }
}

void bt_manager_connect_saved() {
    if (s_bt_connected || !s_has_target_mac) return;
    Serial.printf("[BT] Auto-connecting saved speaker: %s (%s)\n", s_connected_name, s_connected_mac_str);
    bool was_started = s_bt_started;
    bt_ensure_started(true);
    if (was_started) {
        s_a2dp_source.connect_to(s_target_mac);
    }
}

void bt_manager_enter_bluetooth_mode() {
    if (s_bt_connected) {
        mp3_player_play_selected();
        return;
    }
    s_auto_play_on_connect = true;
    bool was_started = s_bt_started;
    if (s_has_target_mac) {
        s_auto_pair_mode = false;
        s_bt_scanning = false;
        s_fallback_scan_pending = true;
        Serial.printf("[BT] Mode -> Bluetooth: connecting saved %s (%s)\n", s_connected_name, s_connected_mac_str);
        bt_ensure_started(true);
        if (was_started) {
            s_a2dp_source.connect_to(s_target_mac);
        }
    } else {
        bt_ensure_started(false);
        s_auto_pair_mode = true;
        s_bt_scanning = true;
        s_scan_start_ms = millis();
        s_scan_timeout_ms = was_started ? 13000UL : 23000UL;
        Serial.println("[BT] Mode -> Bluetooth: discovering speaker to auto-pair...");
        if (was_started) {
            esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 7, 0);
        }
    }
}

void bt_manager_poll() {
    if (s_pending_reconnect) {
        s_pending_reconnect = false;
        Serial.printf("[BT] Link dropped; reconnecting to %s (%s)...\n", s_connected_name, s_connected_mac_str);
        vTaskDelay(pdMS_TO_TICKS(300));
        s_a2dp_source.connect_to(s_target_mac);
    }
    if (s_pending_fallback_scan) {
        s_pending_fallback_scan = false;
        s_auto_pair_mode = true;
        s_bt_scanning = true;
        s_scan_start_ms = millis();
        s_scan_timeout_ms = 15000UL;
        Serial.println("[BT] Saved speaker unreachable; scanning for available speakers...");
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 7, 0);
    }
    if (s_pending_autoplay) {
        s_pending_autoplay = false;
        Serial.println("[BT] Auto-playing selected MP3...");
        mp3_player_play_selected();
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
    }
}

void bt_manager_start_scan() {
    bool was_started = s_bt_started;
    bt_ensure_started();
    if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_bt_device_count = 0;
        xSemaphoreGive(state_mutex);
    }
    s_bt_scanning = true;
    s_scan_start_ms = millis();
    // First start of BluetoothA2DPSource includes a 10s BT_APP_EVT_STACK_UP delay before
    // the library starts discovery itself; avoid calling esp_bt_gap_start_discovery early.
    s_scan_timeout_ms = was_started ? 13000UL : 23000UL;
    if (was_started) {
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 7 /* ~9s */, 0);
    }
}

bool bt_manager_is_scanning() {
    if (s_bt_scanning && (millis() - s_scan_start_ms > s_scan_timeout_ms)) {
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
    app_set_bluetooth_mode();
    s_auto_play_on_connect = true;
    s_bt_scanning = false;

    memset(s_target_mac, 0, sizeof(s_target_mac));
    s_has_target_mac = parse_bt_mac(mac_str, s_target_mac);

    bool was_started = s_bt_started;
    bt_ensure_started(s_has_target_mac);
    if (was_started) {
        esp_bt_gap_cancel_discovery();
        if (s_a2dp_source.is_connected()) {
            s_a2dp_source.disconnect();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

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
        return was_started ? s_a2dp_source.connect_to(s_target_mac) : true;
    }
    // Fallback: start discovery so on_bt_ssid_discovered matches by name
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 7, 0);
    return true;
}

void bt_manager_disconnect() {
    s_bt_scanning = false;
    s_was_connected = false;
    s_pending_reconnect = false;
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
