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

    // 4. Initialize Bluetooth A2DP Source first, then Wi-Fi SoftAP, OLED, and MP3 Player
    Serial.printf("[BOOT] Free Heap before BT: %u (max block: %u)\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    bt_manager_init();
    Serial.printf("[BOOT] Free Heap after BT: %u (max block: %u)\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    web_server_init();
    Serial.printf("[BOOT] Free Heap after WiFi: %u (max block: %u)\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    oled_ui_init();
    mp3_player_init();
    Serial.printf("[BOOT] Free Heap after MP3: %u (max block: %u)\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

    // 5. Pin Application Tasks to Core 1 (APP_CPU) so Core 0 (PRO_CPU) handles Wi-Fi & A2DP cleanly
    xTaskCreatePinnedToCore(mp3_decoder_task, "mp3_dec", 4096, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(web_server_task,  "web_srv", 4096, nullptr, 2, nullptr, 1);

    Serial.printf("[BOOT] Free Heap after init: %u bytes\n", (unsigned)ESP.getFreeHeap());
}

void loop() {
    // Reuse Arduino loopTask (Core 1) for OLED UI to save a 4 KB FreeRTOS task stack
    oled_ui_task(nullptr);
}
