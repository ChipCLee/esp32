#pragma once

#include "types.h"

void rf_scheduler_init();
void rf_scheduler_task(void* param);

// Trigger an immediate out-of-order scan from Web UI or User Button
void rf_scheduler_trigger_scan(scan_state_t mode);
scan_state_t rf_scheduler_get_state();
const char* rf_scheduler_get_state_str();
uint32_t rf_scheduler_get_scan_cycles();

