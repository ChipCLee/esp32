#include "web_server.h"
#include "web_assets.h"
#include "auth_config.h"
#include "device_table.h"
#include "log_buffer.h"
#include "rf_scheduler.h"
#include "sd_logger.h"
#include "display_manager.h"
#include "scanner_wifi.h"
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

