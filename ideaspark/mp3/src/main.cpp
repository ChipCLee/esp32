#include <Arduino.h>
#include <LittleFS.h>
#include <esp_bt.h>

#include "types.h"
#include "mp3_player.h"
#include "bt_manager.h"
#include "oled_ui.h"
#include "web_server.h"

SemaphoreHandle_t fs_mutex = nullptr;
SemaphoreHandle_t state_mutex = nullptr;

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

    // 4. Initialize OLED Display, MP3 Player, Wi-Fi SoftAP Web Server, and Bluetooth A2DP Source
    oled_ui_init();
    mp3_player_init();
    web_server_init();
    bt_manager_init();

    // 5. Pin Application Tasks to Core 1 (APP_CPU) so Core 0 (PRO_CPU) handles Wi-Fi & A2DP cleanly
    xTaskCreatePinnedToCore(mp3_decoder_task, "mp3_dec", 6144, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(web_server_task,  "web_srv", 6144, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(oled_ui_task,     "oled_ui", 4096, nullptr, 1, nullptr, 1);

    Serial.printf("[BOOT] Free Heap after init: %u bytes\n", (unsigned)ESP.getFreeHeap());
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
