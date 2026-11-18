#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_bt.h>

#include "types.h"
#include "mp3_player.h"
#include "bt_manager.h"
#include "oled_ui.h"
#include "web_server.h"

SemaphoreHandle_t fs_mutex = nullptr;
SemaphoreHandle_t state_mutex = nullptr;
static volatile app_mode_t s_app_mode = APP_MODE_WIFI;

app_mode_t app_get_mode() {
    return s_app_mode;
}

void app_set_bluetooth_mode() {
    s_app_mode = APP_MODE_BLUETOOTH;
}

void app_toggle_mode() {
    app_mode_t next_mode = (s_app_mode == APP_MODE_WIFI) ? APP_MODE_BLUETOOTH : APP_MODE_WIFI;
    Serial.printf("[MODE] Switching %s -> %s Mode (restarting for clean DRAM)...\n",
                  s_app_mode == APP_MODE_WIFI ? "Wi-Fi" : "Bluetooth",
                  next_mode == APP_MODE_WIFI ? "Wi-Fi" : "Bluetooth");
    Preferences prefs;
    prefs.begin("app_cfg", false);
    prefs.putUChar("mode", (uint8_t)next_mode);
    prefs.end();

    mp3_player_stop_and_close();
    delay(100);
    ESP.restart();
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n====================================================");
    Serial.println(" Ideaspark ESP32 Wi-Fi MP3 & Bluetooth A2DP Hub");
    Serial.println("====================================================");

    // 1. Release unused BLE controller DRAM before btStart()/A2DP init (~30 KB saved)
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    }

    // 2. Create synchronization mutexes
    fs_mutex = xSemaphoreCreateMutex();
    state_mutex = xSemaphoreCreateMutex();

    // 3. Mount LittleFS (auto-format custom 2.062 MB partition on first boot)
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS mount failed!");
    } else {
        Serial.printf("[FS] LittleFS mounted: %u / %u bytes used\n",
                      (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    }

    // 4. Read persisted boot mode (default: Wi-Fi Config Mode)
    {
        Preferences prefs;
        prefs.begin("app_cfg", true);
        uint8_t m = prefs.getUChar("mode", (uint8_t)APP_MODE_WIFI);
        prefs.end();
        s_app_mode = (m == (uint8_t)APP_MODE_BLUETOOTH) ? APP_MODE_BLUETOOTH : APP_MODE_WIFI;
    }

    bt_manager_init();
    oled_ui_init();
    mp3_player_init();

    if (s_app_mode == APP_MODE_BLUETOOTH) {
        Serial.printf("[BOOT] Mode: BLUETOOTH (free heap=%u, max block=%u)\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        bt_manager_enter_bluetooth_mode();
        xTaskCreatePinnedToCore(mp3_decoder_task, "mp3_dec", 6144, nullptr, 5, nullptr, 1);
    } else {
        Serial.printf("[BOOT] Mode: WIFI (free heap=%u, max block=%u)\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        web_server_init();
        xTaskCreatePinnedToCore(web_server_task,  "web_srv", 6144, nullptr, 2, nullptr, 1);
        xTaskCreatePinnedToCore(mp3_decoder_task, "mp3_dec", 4096, nullptr, 5, nullptr, 1);
    }

    Serial.printf("[BOOT] Free Heap after init: %u bytes\n", (unsigned)ESP.getFreeHeap());
}

void loop() {
    // Reuse Arduino loopTask (Core 1) for OLED UI to save a 4 KB FreeRTOS task stack
    oled_ui_task(nullptr);
}
