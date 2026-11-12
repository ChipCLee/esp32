#pragma once

#include "types.h"

void scanner_wifi_init();
// Scans a micro-batch of 2 Wi-Fi channels (<= 90ms off-channel) to preserve SoftAP stability
void scanner_wifi_scan_step(uint8_t start_channel, uint8_t num_channels);
// Sweeps all 13 channels using channel-interleaved micro-windows + 100ms SoftAP recovery
void scanner_wifi_full_sweep();
uint8_t scanner_wifi_get_connected_users();
void scanner_wifi_check_client_events();

