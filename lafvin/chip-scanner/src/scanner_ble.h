#pragma once

#include "types.h"

void scanner_ble_init();
// Runs a coexistence-safe active BLE 5.3 & Matter discovery window
void scanner_ble_scan_window(uint32_t duration_ms);

