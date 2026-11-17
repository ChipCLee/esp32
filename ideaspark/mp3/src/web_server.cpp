#include "web_server.h"
#include "web_assets.h"
#include "mp3_player.h"
#include "bt_manager.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static WebServer s_server(80);
static File s_upload_file;
static bool s_upload_active = false;
static bool s_upload_error = false;
static uint16_t s_upload_http_code = 200;
static char s_upload_msg[96] = {0};
static char s_upload_path[32] = {0};

// Sanitize & truncate uploaded filename to <= 31 chars total (including leading '/')
static void sanitize_mp3_path(const char* raw_name, char* out_path, size_t out_len) {
    const char* base = strrchr(raw_name, '/');
    base = base ? (base + 1) : raw_name;
    const char* b2 = strrchr(base, '\\');
    if (b2) base = b2 + 1;

    char clean[28] = {0};
    size_t idx = 0;
    for (size_t i = 0; base[i] != '\0' && idx < 23; ++i) {
        char c = base[i];
        if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') {
            clean[idx++] = c;
        } else if (c == ' ') {
            clean[idx++] = '_';
        }
    }
    clean[idx] = '\0';

    // Strip trailing .mp3 if present so we can cleanly re-append ".mp3"
    size_t clen = strlen(clean);
    if (clen > 4 && strcasecmp(clean + clen - 4, ".mp3") == 0) {
        clean[clen - 4] = '\0';
    }
    if (clean[0] == '\0') {
        strcpy(clean, "track");
    }

    snprintf(out_path, out_len, "/%.26s.mp3", clean);
    // Deduplicate if file already exists
    if (LittleFS.exists(out_path)) {
        for (int n = 1; n <= 99; ++n) {
            snprintf(out_path, out_len, "/%.23s_%d.mp3", clean, n);
            if (!LittleFS.exists(out_path)) break;
        }
    }
}

static void handle_root() {
    s_server.sendHeader("Content-Encoding", "gzip");
    s_server.sendHeader("Cache-Control", "no-cache");
    s_server.send_P(200, "text/html", (const char*)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
}

static void handle_api_status() {
    char path[32], title[36], artist[36], bt_name[36], bt_mac[18];
    mp3_player_get_now_playing(path, sizeof(path), title, sizeof(title), artist, sizeof(artist));
    uint8_t vu_l = 0, vu_r = 0;
    mp3_player_get_vu(&vu_l, &vu_r);

    bool bt_conn = false, bt_scan = false;
    bt_manager_get_status(&bt_conn, &bt_scan, bt_name, sizeof(bt_name), bt_mac, sizeof(bt_mac));

    player_state_t st = mp3_player_get_state();
    const char* st_str = (st == PLAYER_PLAYING) ? "PLAYING" : (st == PLAYER_PAUSED ? "PAUSED" : "STOPPED");

    JsonDocument doc;
    doc["state"] = st_str;
    doc["file"] = path;
    doc["title"] = title;
    doc["artist"] = artist;
    doc["volume"] = mp3_player_get_volume();
    doc["vu_l"] = vu_l;
    doc["vu_r"] = vu_r;
    doc["bt_connected"] = bt_conn;
    doc["bt_scanning"] = bt_scan;
    doc["bt_name"] = bt_name;
    doc["bt_mac"] = bt_mac;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["fs_used"] = LittleFS.usedBytes();
    doc["fs_total"] = LittleFS.totalBytes();

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_api_mp3_list() {
    static mp3_track_t s_list_buf[MAX_TRACKS];
    size_t cnt = mp3_player_get_tracks(s_list_buf, MAX_TRACKS);

    JsonDocument doc;
    JsonArray arr = doc["tracks"].to<JsonArray>();
    for (size_t i = 0; i < cnt; ++i) {
        JsonObject obj = arr.add<JsonObject>();
        obj["path"] = s_list_buf[i].path;
        obj["title"] = s_list_buf[i].title;
        obj["artist"] = s_list_buf[i].artist;
        obj["size"] = s_list_buf[i].size_bytes;
    }

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_api_mp3_upload_data() {
    HTTPUpload& upload = s_server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        s_upload_active = true;
        s_upload_error = false;
        s_upload_http_code = 200;
        s_upload_msg[0] = '\0';

        // Stop playback & close open file handle before writing Flash (MP3-NFR-4)
        mp3_player_stop_and_close();

        if (fs_mutex && xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            sanitize_mp3_path(upload.filename.c_str(), s_upload_path, sizeof(s_upload_path));
            size_t free_bytes = LittleFS.totalBytes() - LittleFS.usedBytes();
            if (free_bytes < 8192) {
                s_upload_error = true;
                s_upload_http_code = 507;
                snprintf(s_upload_msg, sizeof(s_upload_msg), "LittleFS storage full");
            } else {
                s_upload_file = LittleFS.open(s_upload_path, "w");
                if (!s_upload_file) {
                    s_upload_error = true;
                    s_upload_http_code = 500;
                    snprintf(s_upload_msg, sizeof(s_upload_msg), "Failed to open file in LittleFS");
                }
            }
            xSemaphoreGive(fs_mutex);
        } else {
            s_upload_error = true;
            s_upload_http_code = 503;
            snprintf(s_upload_msg, sizeof(s_upload_msg), "Filesystem busy");
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!s_upload_error && s_upload_file) {
            if (fs_mutex && xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                size_t free_bytes = LittleFS.totalBytes() - LittleFS.usedBytes();
                if (upload.currentSize > free_bytes) {
                    s_upload_error = true;
                    s_upload_http_code = 507;
                    snprintf(s_upload_msg, sizeof(s_upload_msg), "Insufficient LittleFS space");
                    s_upload_file.close();
                    LittleFS.remove(s_upload_path);
                } else {
                    size_t written = s_upload_file.write(upload.buf, upload.currentSize);
                    if (written != upload.currentSize) {
                        s_upload_error = true;
                        s_upload_http_code = 507;
                        snprintf(s_upload_msg, sizeof(s_upload_msg), "Flash write error");
                        s_upload_file.close();
                        LittleFS.remove(s_upload_path);
                    }
                }
                xSemaphoreGive(fs_mutex);
            }
        }
    } else if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
        if (fs_mutex && xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            if (s_upload_file) {
                s_upload_file.close();
            }
            if (upload.status == UPLOAD_FILE_ABORTED || s_upload_error) {
                LittleFS.remove(s_upload_path);
                if (!s_upload_error) {
                    s_upload_error = true;
                    s_upload_http_code = 400;
                    snprintf(s_upload_msg, sizeof(s_upload_msg), "Upload aborted");
                }
            }
            xSemaphoreGive(fs_mutex);
        }
        if (!s_upload_error) {
            mp3_player_refresh_library();
        }
        s_upload_active = false;
    }
}

static void handle_api_mp3_upload_done() {
    if (s_upload_error) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["error"] = s_upload_msg;
        String out;
        serializeJson(doc, out);
        s_server.send(s_upload_http_code, "application/json", out);
        return;
    }

    JsonDocument doc;
    doc["status"] = "uploaded";
    doc["path"] = s_upload_path;
    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_api_mp3_delete() {
    JsonDocument req;
    if (deserializeJson(req, s_server.arg("plain"))) {
        s_server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    const char* target = req["file"] | "";
    if (target[0] == '\0') {
        s_server.send(400, "application/json", "{\"error\":\"Missing file\"}");
        return;
    }

    char cur_path[32] = {0};
    mp3_player_get_now_playing(cur_path, sizeof(cur_path), nullptr, 0, nullptr, 0);
    if (strcmp(cur_path, target) == 0) {
        mp3_player_stop_and_close();
    }

    bool removed = false;
    if (fs_mutex && xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        removed = LittleFS.remove(target);
        xSemaphoreGive(fs_mutex);
    }

    if (removed) {
        mp3_player_refresh_library();
        s_server.send(200, "application/json", "{\"status\":\"deleted\"}");
    } else {
        s_server.send(404, "application/json", "{\"error\":\"File not found\"}");
    }
}

static void handle_api_player_control() {
    JsonDocument req;
    if (deserializeJson(req, s_server.arg("plain"))) {
        s_server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    const char* action = req["action"] | "";
    const char* file   = req["file"] | "";

    if (strcmp(action, "play") == 0) {
        if (file[0] != '\0') mp3_player_play(file);
        else                 mp3_player_resume();
    } else if (strcmp(action, "pause") == 0) {
        mp3_player_pause();
    } else if (strcmp(action, "resume") == 0) {
        mp3_player_resume();
    } else if (strcmp(action, "toggle") == 0) {
        mp3_player_toggle_pause();
    } else if (strcmp(action, "stop") == 0) {
        mp3_player_stop_and_close();
    } else if (strcmp(action, "next") == 0) {
        mp3_player_next();
    } else if (strcmp(action, "prev") == 0) {
        mp3_player_prev();
    } else if (strcmp(action, "volume") == 0 && req["volume"].is<int>()) {
        int vol = req["volume"].as<int>();
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        mp3_player_set_volume((uint8_t)vol);
    } else {
        s_server.send(400, "application/json", "{\"error\":\"Unknown action\"}");
        return;
    }

    s_server.send(200, "application/json", "{\"status\":\"ok\"}");
}

static void handle_api_bt_scan() {
    bt_manager_start_scan();
    s_server.send(200, "application/json", "{\"status\":\"scanning\"}");
}

static void handle_api_bt_devices() {
    bt_device_info_t devs[MAX_BT_DEVICES];
    size_t cnt = bt_manager_get_devices(devs, MAX_BT_DEVICES);

    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    for (size_t i = 0; i < cnt; ++i) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = devs[i].name;
        obj["mac"]  = devs[i].mac_str;
        obj["rssi"] = devs[i].rssi;
    }

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_api_bt_connect() {
    JsonDocument req;
    if (deserializeJson(req, s_server.arg("plain"))) {
        s_server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    const char* name = req["name"] | "";
    const char* mac  = req["mac"] | "";
    if (name[0] == '\0' && mac[0] == '\0') {
        s_server.send(400, "application/json", "{\"error\":\"Provide speaker name or MAC\"}");
        return;
    }
    bt_manager_connect(name, mac);
    s_server.send(200, "application/json", "{\"status\":\"connecting\"}");
}

static void handle_api_bt_disconnect() {
    bt_manager_disconnect();
    s_server.send(200, "application/json", "{\"status\":\"disconnected\"}");
}

void web_server_init() {
    WiFi.mode(WIFI_AP);
    IPAddress ip(192, 168, 4, 1);
    IPAddress gw(192, 168, 4, 1);
    IPAddress mask(255, 255, 255, 0);
    WiFi.softAPConfig(ip, gw, mask);
    WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, 4);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    s_server.on("/", HTTP_GET, handle_root);
    s_server.on("/api/status", HTTP_GET, handle_api_status);
    s_server.on("/api/mp3/list", HTTP_GET, handle_api_mp3_list);
    s_server.on("/api/mp3/upload", HTTP_POST, handle_api_mp3_upload_done, handle_api_mp3_upload_data);
    s_server.on("/api/mp3/delete", HTTP_POST, handle_api_mp3_delete);
    s_server.on("/api/player/control", HTTP_POST, handle_api_player_control);
    s_server.on("/api/bt/scan", HTTP_POST, handle_api_bt_scan);
    s_server.on("/api/bt/devices", HTTP_GET, handle_api_bt_devices);
    s_server.on("/api/bt/connect", HTTP_POST, handle_api_bt_connect);
    s_server.on("/api/bt/disconnect", HTTP_POST, handle_api_bt_disconnect);

    s_server.begin();
    Serial.printf("[WIFI] SoftAP '%s' ready at http://192.168.4.1\n", AP_SSID);
}

void web_server_task(void* param) {
    (void)param;
    for (;;) {
        s_server.handleClient();
        // Yield minimally during active MP3 uploads for high throughput (MP3-NFR-5)
        vTaskDelay(pdMS_TO_TICKS(s_upload_active ? 1 : 5));
    }
}
