#include "rf_scheduler.h"
#include "auth_config.h"
#include "scanner_wifi.h"
#include "scanner_ble.h"
#include "scanner_154.h"
#include "led_status.h"
#include "log_buffer.h"

static volatile scan_state_t s_current_state = SCAN_STATE_IDLE;
static volatile scan_state_t s_pending_request = SCAN_STATE_IDLE;
static volatile uint32_t s_scan_cycles = 0;

void rf_scheduler_init() {
    s_current_state = SCAN_STATE_IDLE;
    s_pending_request = SCAN_STATE_ALL; // Run an initial full sweep right after boot
}

void rf_scheduler_trigger_scan(scan_state_t mode) {
    s_pending_request = mode;
}

scan_state_t rf_scheduler_get_state() {
    return s_current_state;
}

const char* rf_scheduler_get_state_str() {
    switch (s_current_state) {
        case SCAN_STATE_IDLE:   return "IDLE";
        case SCAN_STATE_WIFI:   return "WIFI";
        case SCAN_STATE_BLE:    return "BLE";
        case SCAN_STATE_ZIGBEE: return "15.4";
        case SCAN_STATE_ALL:    return "SWEEP";
        default:                return "IDLE";
    }
}

uint32_t rf_scheduler_get_scan_cycles() {
    return s_scan_cycles;
}

static void run_ble_sweep() {
    s_current_state = SCAN_STATE_BLE;
    led_status_set(LED_STATE_SCAN_BLE);
    // Two 450ms coexistence-safe active BLE bursts separated by 100ms Wi-Fi AP recovery
    scanner_ble_scan_window(450);
    vTaskDelay(pdMS_TO_TICKS(100));
    scanner_ble_scan_window(450);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void run_wifi_sweep() {
    s_current_state = SCAN_STATE_WIFI;
    led_status_set(LED_STATE_SCAN_WIFI);
    scanner_wifi_full_sweep();
}

static void run_154_sweep() {
    s_current_state = SCAN_STATE_ZIGBEE;
    led_status_set(LED_STATE_SCAN_154);
    scanner_154_full_sweep();
}

void rf_scheduler_task(void* param) {
    (void)param;
    uint32_t last_auto_scan_ms = millis();
    uint8_t round_robin_slot = 0;

    for (;;) {
        scanner_wifi_check_client_events();
        scanner_154_process_rx_queue();

        scan_state_t req = s_pending_request;
        if (req != SCAN_STATE_IDLE) {
            s_pending_request = SCAN_STATE_IDLE;

            if (req == SCAN_STATE_WIFI) {
                log_buffer_push(PROTO_SYSTEM, "Manual Wi-Fi scan started");
                run_wifi_sweep();
            } else if (req == SCAN_STATE_BLE) {
                log_buffer_push(PROTO_SYSTEM, "Manual BLE scan started");
                run_ble_sweep();
            } else if (req == SCAN_STATE_ZIGBEE) {
                log_buffer_push(PROTO_SYSTEM, "Manual 802.15.4 scan started");
                run_154_sweep();
            } else if (req == SCAN_STATE_ALL) {
                log_buffer_push(PROTO_SYSTEM, "Full spectrum sweep started");
                run_ble_sweep();
                run_wifi_sweep();
                run_154_sweep();
            }

            s_scan_cycles++;
            s_current_state = SCAN_STATE_IDLE;
            led_status_set(LED_STATE_IDLE_AP);
            last_auto_scan_ms = millis();
        } else if (g_config.auto_scan) {
            uint32_t interval_ms = (uint32_t)g_config.interval_s * 1000UL;
            if (interval_ms < 5000UL) interval_ms = 5000UL;

            uint32_t now = millis();
            if (now - last_auto_scan_ms >= (interval_ms / 3)) {
                // Time-sliced round-robin slot execution (Section 3.2)
                if (round_robin_slot == 0) {
                    run_ble_sweep();
                } else if (round_robin_slot == 1) {
                    run_wifi_sweep();
                } else {
                    run_154_sweep();
                    s_scan_cycles++;
                }
                round_robin_slot = (round_robin_slot + 1) % 3;
                s_current_state = SCAN_STATE_IDLE;
                led_status_set(LED_STATE_IDLE_AP);
                last_auto_scan_ms = millis();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

