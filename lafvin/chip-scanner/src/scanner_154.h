#pragma once

#include "types.h"

void scanner_154_init();
// Scans a micro-batch of 802.15.4 channels (e.g. 2 channels @ 35ms each <= 70ms off-channel)
void scanner_154_scan_step(uint8_t start_channel, uint8_t num_channels);
// Sweeps all 16 IEEE 802.15.4 channels (11..26) with 100ms SoftAP recovery between batches
void scanner_154_full_sweep();
// Drains ISR ring buffer and upserts discovered Zigbee/Thread nodes into device_table
void scanner_154_process_rx_queue();

