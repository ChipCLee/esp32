#include "web_server.h"
#include "web_assets.h"
#include "auth_config.h"
#include "device_table.h"
#include "log_buffer.h"
#include "rf_scheduler.h"
#include "sd_logger.h"
#include "display_manager.h"
#include "scanner_wifi.h"
#include "scanner_ble.h"
#include "led_status.h"

#include <WebServer.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>

static WebServer s_server(80);
static discovered_device_t s_dev_snapshot[MAX_DISCOVERED_DEVICES];

static const char* proto_to_json_str(rf_protocol_t p) {
    switch (p) {
        case PROTO_WIFI:   return "WIFI";
        case PROTO_BLE:    return "BLE";
        case PROTO_ZIGBEE: return "ZIGBEE";
        case PROTO_MATTER: return "MATTER";
        case PROTO_SYSTEM: return "SYS";
        default:           return "SYS";
    }
}

// Custom HTTP Basic Authentication helper supporting constant-time verification and brute-force lockout (FR-11, NFR-7)
static bool check_http_auth() {
    String client_ip = s_server.client().remoteIP().toString();

    if (auth_is_locked_out()) {
        led_status_set(LED_STATE_ERROR);
        s_server.sendHeader("WWW-Authenticate", "Basic realm=\"chip-scanner\"");
        s_server.send(401, "application/json", "{\"error\":\"Too many failed attempts. Locked out for 5s.\"}");
        return false;
    }

    if (!s_server.hasHeader("Authorization")) {
        s_server.sendHeader("WWW-Authenticate", "Basic realm=\"chip-scanner\"");
        s_server.send(401, "application/json", "{\"error\":\"Authentication required\"}");
        return false;
    }

    String auth_header = s_server.header("Authorization");
    if (!auth_header.startsWith("Basic ")) {
        s_server.sendHeader("WWW-Authenticate", "Basic realm=\"chip-scanner\"");
        s_server.send(401, "application/json", "{\"error\":\"Invalid authorization scheme\"}");
        return false;
    }

    String b64 = auth_header.substring(6);
    b64.trim();

    unsigned char decoded[96] = {0};
    size_t out_len = 0;
    int rc = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &out_len,
                                   (const unsigned char*)b64.c_str(), b64.length());
    if (rc != 0 || out_len == 0) {
        s_server.sendHeader("WWW-Authenticate", "Basic realm=\"chip-scanner\"");
        s_server.send(401, "application/json", "{\"error\":\"Malformed credentials\"}");
        return false;
    }
    decoded[out_len] = '\0';

    char* colon = strchr((char*)decoded, ':');
    if (!colon) {
        s_server.sendHeader("WWW-Authenticate", "Basic realm=\"chip-scanner\"");
        s_server.send(401, "application/json", "{\"error\":\"Malformed credentials\"}");
        return false;
    }

    *colon = '\0';
    const char* user = (const char*)decoded;
    const char* pass = colon + 1;

    if (!auth_verify_credentials(user, pass, client_ip.c_str())) {
        if (auth_is_locked_out()) {
            led_status_set(LED_STATE_ERROR);
        }
        s_server.sendHeader("WWW-Authenticate", "Basic realm=\"chip-scanner\"");
        s_server.send(401, "application/json", "{\"error\":\"Invalid username or password\"}");
        return false;
    }

    return true;
}

static void handle_root() {
    if (!check_http_auth()) return;
    s_server.sendHeader("Content-Encoding", "gzip");
    s_server.sendHeader("Cache-Control", "no-cache");
    s_server.send_P(200, "text/html", (const char*)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
}

static void handle_api_devices() {
    if (!check_http_auth()) return;

    size_t count = device_table_snapshot(s_dev_snapshot, MAX_DISCOVERED_DEVICES);
    uint32_t now_ms = millis();

    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();

    for (size_t i = 0; i < count; i++) {
        const discovered_device_t& d = s_dev_snapshot[i];
        JsonObject obj = arr.add<JsonObject>();
        obj["proto"] = proto_to_json_str(d.protocol);
        obj["name"] = d.name;
        char id_str[28];
        format_mac_or_id(d.protocol, d.mac_or_id, id_str, sizeof(id_str));
        obj["id"] = id_str;
        obj["ch"] = d.channel;
        obj["rssi"] = d.rssi;
        obj["details"] = d.details;
        obj["connectable"] = d.ble_connectable;
        obj["addr_type"] = d.ble_addr_type;
        uint32_t age = (now_ms >= d.last_seen_ms) ? ((now_ms - d.last_seen_ms) / 1000) : 0;
        obj["age"] = age;
    }

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_api_stats() {
    if (!check_http_auth()) return;

    protocol_counts_t counts = device_table_get_counts();
    JsonDocument doc;
    doc["connected_users"] = scanner_wifi_get_connected_users();
    doc["wifi_count"] = counts.wifi_count;
    doc["ble_count"] = counts.ble_count;
    doc["zigbee_count"] = counts.zigbee_count;
    doc["matter_count"] = counts.matter_count;
    doc["total_count"] = counts.total_count;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["uptime_s"] = millis() / 1000;
    doc["scan_cycles"] = rf_scheduler_get_scan_cycles();
    doc["scan_state"] = rf_scheduler_get_state_str();
    doc["sd_ready"] = sd_logger_is_ready();
    doc["auto_scan"] = g_config.auto_scan;
    doc["interval_s"] = g_config.interval_s;
    doc["lcd_backlight"] = display_manager_get_backlight();

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_api_logs() {
    if (!check_http_auth()) return;

    lcd_log_entry_t entries[LCD_LOG_HISTORY_SIZE];
    size_t count = log_buffer_get_recent(entries, LCD_LOG_HISTORY_SIZE);

    JsonDocument doc;
    JsonArray arr = doc["logs"].to<JsonArray>();

    for (size_t i = 0; i < count; i++) {
        JsonObject obj = arr.add<JsonObject>();
        char ts_str[16];
        log_format_timestamp(entries[i], ts_str, sizeof(ts_str));
        obj["ts_str"] = ts_str;
        obj["ts"] = entries[i].timestamp_s;
        obj["proto"] = proto_to_json_str(entries[i].protocol);
        obj["msg"] = entries[i].message;
    }

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_api_scan() {
    if (!check_http_auth()) return;

    String body = s_server.arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        s_server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    const char* proto = doc["protocol"] | "all";
    scan_state_t target = SCAN_STATE_ALL;
    if (strcasecmp(proto, "wifi") == 0) {
        target = SCAN_STATE_WIFI;
    } else if (strcasecmp(proto, "ble") == 0) {
        target = SCAN_STATE_BLE;
    } else if (strcasecmp(proto, "zigbee") == 0) {
        target = SCAN_STATE_ZIGBEE;
    } else {
        target = SCAN_STATE_ALL;
    }

    rf_scheduler_trigger_scan(target);
    log_buffer_push(PROTO_SYSTEM, "Web UI triggered %s scan", proto);
    s_server.send(200, "application/json", "{\"status\":\"started\"}");
}

static void handle_api_config() {
    if (!check_http_auth()) return;

    String body = s_server.arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        s_server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    bool save_needed = false;

    if (doc["epoch_s"].is<uint32_t>()) {
        uint32_t epoch = doc["epoch_s"].as<uint32_t>();
        log_sync_epoch(epoch);
    }

    if (doc["auto_scan"].is<bool>()) {
        g_config.auto_scan = doc["auto_scan"].as<bool>();
        log_buffer_push(PROTO_SYSTEM, "Auto-scan %s", g_config.auto_scan ? "enabled" : "paused");
        save_needed = true;
    }

    if (doc["interval_s"].is<uint16_t>()) {
        uint16_t iv = doc["interval_s"].as<uint16_t>();
        if (iv >= 1 && iv <= 300) {
            g_config.interval_s = iv;
            save_needed = true;
        }
    }

    if (doc["lcd_backlight"].is<bool>()) {
        bool bl = doc["lcd_backlight"].as<bool>();
        display_manager_set_backlight(bl);
        save_needed = true;
    }

    if (doc["new_password"].is<const char*>()) {
        const char* np = doc["new_password"].as<const char*>();
        if (np && strlen(np) >= 4 && strlen(np) < sizeof(g_config.http_pass)) {
            strncpy(g_config.http_pass, np, sizeof(g_config.http_pass) - 1);
            g_config.http_pass[sizeof(g_config.http_pass) - 1] = '\0';
            log_buffer_push(PROTO_SYSTEM, "Admin password updated");
            save_needed = true;
        }
    }

    if (doc["ap_password"].is<const char*>()) {
        const char* ap = doc["ap_password"].as<const char*>();
        if (ap && strlen(ap) >= 8 && strlen(ap) < sizeof(g_config.ap_pass)) {
            strncpy(g_config.ap_pass, ap, sizeof(g_config.ap_pass) - 1);
            g_config.ap_pass[sizeof(g_config.ap_pass) - 1] = '\0';
            log_buffer_push(PROTO_SYSTEM, "Wi-Fi AP password updated");
            save_needed = true;
        }
    }

    if (save_needed) {
        auth_config_save();
    }

    s_server.send(200, "application/json", "{\"status\":\"ok\"}");
}

static void handle_api_clear() {
    if (!check_http_auth()) return;

    device_table_clear();
    log_buffer_clear();
    s_server.send(200, "application/json", "{\"status\":\"cleared\"}");
}

static void handle_api_export_csv() {
    if (!check_http_auth()) return;

    String csv = sd_logger_export_csv();
    s_server.sendHeader("Content-Disposition", "attachment; filename=\"discovery.csv\"");
    s_server.send(200, "text/csv", csv);
}

static void populate_prop_array(JsonArray& props_arr, uint8_t mask) {
    if (mask & BLE_PROP_READ)      props_arr.add("READ");
    if (mask & BLE_PROP_WRITE)     props_arr.add("WRITE");
    if (mask & BLE_PROP_WRITE_NR)  props_arr.add("WRITE_NR");
    if (mask & BLE_PROP_NOTIFY)    props_arr.add("NOTIFY");
    if (mask & BLE_PROP_INDICATE)  props_arr.add("INDICATE");
    if (mask & BLE_PROP_BROADCAST) props_arr.add("BROADCAST");
    if (mask & BLE_PROP_AUTH_SIGN) props_arr.add("AUTH_SIGN");
    if (mask & BLE_PROP_EXTENDED)  props_arr.add("EXT");
}

static void handle_api_ble_status() {
    if (!check_http_auth()) return;

    bool connected = false;
    uint8_t mac[6] = {0};
    char name[33] = {0};
    int8_t rssi = 0;
    uint16_t mtu = 0;
    uint32_t idle_s = 0;
    scanner_ble_get_gatt_status(&connected, mac, name, sizeof(name), &rssi, &mtu, &idle_s);

    JsonDocument doc;
    doc["connected"] = connected;
    if (connected) {
        char mac_str[20];
        format_mac_or_id(PROTO_BLE, mac, mac_str, sizeof(mac_str));
        doc["mac"] = mac_str;
        doc["name"] = name;
        doc["rssi"] = rssi;
        doc["mtu"] = mtu;
        doc["idle_s"] = idle_s;
    }

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_api_ble_discover() {
    if (!check_http_auth()) return;

    String body = s_server.arg("plain");
    JsonDocument req_doc;
    if (deserializeJson(req_doc, body)) {
        s_server.send(400, "application/json", "{\"status\":\"error\",\"error\":\"Invalid JSON payload\"}");
        return;
    }

    const char* mac_str = req_doc["mac"] | "";
    uint8_t mac[6] = {0};
    if (!parse_mac_str(mac_str, mac)) {
        s_server.send(400, "application/json", "{\"status\":\"error\",\"error\":\"Invalid or missing BLE MAC address\"}");
        return;
    }

    int addr_type_override = -1;
    if (req_doc["addr_type"].is<int>()) {
        addr_type_override = req_doc["addr_type"].as<int>();
    }

    rf_scheduler_begin_gatt();
    const ble_gatt_discovery_result_t* res = scanner_ble_discover_gatt(mac, addr_type_override);

    JsonDocument doc;
    char norm_mac[20];
    format_mac_or_id(PROTO_BLE, res->mac, norm_mac, sizeof(norm_mac));
    doc["mac"] = norm_mac;

    if (!res->success) {
        doc["status"] = "error";
        doc["connected"] = false;
        doc["error"] = res->error_msg[0] ? res->error_msg : "BLE GATT discovery failed";
        rf_scheduler_end_gatt();
        String out;
        serializeJson(doc, out);
        s_server.send(200, "application/json", out);
        return;
    }

    doc["status"] = "ok";
    doc["connected"] = res->connected;
    doc["addr_type"] = res->addr_type;
    doc["name"] = res->device_name;
    doc["rssi"] = res->rssi;
    doc["mtu"] = res->mtu;
    doc["service_count"] = res->service_count;
    doc["char_count"] = res->total_char_count;

    JsonArray svcs_arr = doc["services"].to<JsonArray>();
    for (uint8_t s = 0; s < res->service_count; ++s) {
        const ble_gatt_service_info_t& svc = res->services[s];
        JsonObject svc_obj = svcs_arr.add<JsonObject>();
        svc_obj["uuid"] = svc.uuid;
        svc_obj["start_handle"] = svc.start_handle;
        svc_obj["end_handle"] = svc.end_handle;

        JsonArray chrs_arr = svc_obj["characteristics"].to<JsonArray>();
        for (uint8_t c = 0; c < svc.char_count; ++c) {
            uint8_t idx = svc.char_start_idx + c;
            if (idx >= res->total_char_count) break;
            const ble_gatt_char_info_t& chr = res->chars[idx];
            JsonObject chr_obj = chrs_arr.add<JsonObject>();
            chr_obj["uuid"] = chr.uuid;
            chr_obj["handle"] = chr.handle;
            chr_obj["props_raw"] = chr.properties;
            chr_obj["can_read"] = (bool)(chr.properties & BLE_PROP_READ);
            JsonArray p_arr = chr_obj["properties"].to<JsonArray>();
            populate_prop_array(p_arr, chr.properties);
        }
    }

    rf_scheduler_end_gatt();

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_api_ble_read() {
    if (!check_http_auth()) return;

    String body = s_server.arg("plain");
    JsonDocument req_doc;
    if (deserializeJson(req_doc, body)) {
        s_server.send(400, "application/json", "{\"status\":\"error\",\"error\":\"Invalid JSON payload\"}");
        return;
    }

    const char* mac_str = req_doc["mac"] | "";
    uint8_t mac[6] = {0};
    if (!parse_mac_str(mac_str, mac)) {
        s_server.send(400, "application/json", "{\"status\":\"error\",\"error\":\"Invalid or missing BLE MAC address\"}");
        return;
    }

    int addr_type_override = -1;
    if (req_doc["addr_type"].is<int>()) {
        addr_type_override = req_doc["addr_type"].as<int>();
    }
    const char* service_uuid = req_doc["service_uuid"] | "";
    const char* char_uuid = req_doc["char_uuid"] | "";
    uint16_t handle = req_doc["handle"] | 0;

    rf_scheduler_begin_gatt();
    const ble_gatt_read_result_t* res = scanner_ble_read_characteristic(
        mac, addr_type_override, service_uuid, char_uuid, handle);

    JsonDocument doc;
    char norm_mac[20];
    format_mac_or_id(PROTO_BLE, res->mac, norm_mac, sizeof(norm_mac));
    doc["mac"] = norm_mac;

    if (!res->success) {
        doc["status"] = "error";
        doc["error"] = res->error_msg[0] ? res->error_msg : "Characteristic read failed";
        rf_scheduler_end_gatt();
        String out;
        serializeJson(doc, out);
        s_server.send(200, "application/json", out);
        return;
    }

    doc["status"] = "ok";
    doc["service_uuid"] = res->service_uuid;
    doc["char_uuid"] = res->char_uuid;
    doc["handle"] = res->handle;
    doc["rssi"] = res->rssi;
    doc["length"] = res->value_len;
    doc["hex"] = res->hex_str;
    doc["utf8"] = res->utf8_str;
    doc["decoded"] = res->decoded_str;

    rf_scheduler_end_gatt();

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_api_ble_disconnect() {
    if (!check_http_auth()) return;

    scanner_ble_disconnect_gatt();
    s_server.send(200, "application/json", "{\"status\":\"disconnected\"}");
}

void web_server_init() {
    const char* header_keys[] = {"Authorization"};
    s_server.collectHeaders(header_keys, 1);

    s_server.on("/", HTTP_GET, handle_root);
    s_server.on("/api/devices", HTTP_GET, handle_api_devices);
    s_server.on("/api/stats", HTTP_GET, handle_api_stats);
    s_server.on("/api/logs", HTTP_GET, handle_api_logs);
    s_server.on("/api/scan", HTTP_POST, handle_api_scan);
    s_server.on("/api/config", HTTP_POST, handle_api_config);
    s_server.on("/api/clear", HTTP_POST, handle_api_clear);
    s_server.on("/api/export.csv", HTTP_GET, handle_api_export_csv);
    s_server.on("/api/ble/status", HTTP_GET, handle_api_ble_status);
    s_server.on("/api/ble/discover", HTTP_POST, handle_api_ble_discover);
    s_server.on("/api/ble/read", HTTP_POST, handle_api_ble_read);
    s_server.on("/api/ble/disconnect", HTTP_POST, handle_api_ble_disconnect);

    s_server.onNotFound([]() {
        if (!check_http_auth()) return;
        s_server.send(404, "application/json", "{\"error\":\"Not found\"}");
    });

    s_server.begin();
    log_buffer_push(PROTO_SYSTEM, "HTTP server ready :80");
}

void web_server_task(void* param) {
    (void)param;
    while (true) {
        s_server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

