#include <Arduino.h>
#include <SPI.h>

#include "types.h"
#include "auth_config.h"
#include "log_buffer.h"
#include "device_table.h"
#include "led_status.h"
#include "display_manager.h"
#include "sd_logger.h"
#include "scanner_wifi.h"
#include "scanner_ble.h"
#include "scanner_154.h"
#include "rf_scheduler.h"
#include "web_server.h"

// Global synchronization primitives (Section 3.3 & Section 3.4)
SemaphoreHandle_t data_table_mutex = nullptr;
SemaphoreHandle_t spi_bus_mutex    = nullptr;

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=======================================================");
    Serial.println(" LAFVIN ESP32-C6 Multi-Protocol RF Discovery & Web Portal");
    Serial.println("=======================================================");

    // 1. Pre-initialize shared FSPI Chip Selects HIGH before SPI.begin() (Section 3.4)
    pinMode(PIN_LCD_CS, OUTPUT);
    digitalWrite(PIN_LCD_CS, HIGH);
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);

    // Initialize shared hardware FSPI bus (SCK=7, MISO=5, MOSI=6)
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);

    // 2. Create thread synchronization mutexes
    data_table_mutex = xSemaphoreCreateMutex();
    spi_bus_mutex    = xSemaphoreCreateMutex();

    // 3. Initialize core configuration, logging, device table, and status LED
    auth_config_init();
    log_buffer_init();
    device_table_init();
    led_status_init();

    // 4. Initialize shared SPI peripherals (1.47" ST7789 LCD & Micro-SD Logger)
    display_manager_init(&SPI);
    sd_logger_init(&SPI);

    // 5. Initialize RF Protocol Scanners (Wi-Fi 6 SoftAP+Scan, BLE 5.3, IEEE 802.15.4)
    scanner_wifi_init();
    scanner_ble_init();
    scanner_154_init();
    rf_scheduler_init();

    // 6. Initialize Password-Protected HTTP Server
    web_server_init();

    // 7. Launch FreeRTOS Tasks (ESP32-C6 single-core RISC-V @ 160MHz)
    xTaskCreate(web_server_task,      "web_srv",   8192, nullptr, 4, nullptr);
    xTaskCreate(rf_scheduler_task,    "rf_sched",  8192, nullptr, 3, nullptr);
    xTaskCreate(display_manager_task, "lcd_mgr",   4096, nullptr, 2, nullptr);
    xTaskCreate(sd_logger_task,       "sd_log",    4096, nullptr, 1, nullptr);

    Serial.printf("[BOOT] Free Heap after init: %u bytes\n", (unsigned)ESP.getFreeHeap());
}

void loop() {
    // Housekeeping & LED breathing/lockout status indicator
    led_status_tick();
    vTaskDelay(pdMS_TO_TICKS(250));
}

