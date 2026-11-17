#pragma once

#include "types.h"

void bt_manager_init();
void bt_manager_start_scan();
bool bt_manager_is_scanning();
size_t bt_manager_get_devices(bt_device_info_t* out_devs, size_t max_devs);

bool bt_manager_connect(const char* name, const char* mac_str);
void bt_manager_disconnect();

bool bt_manager_is_connected();
void bt_manager_get_status(bool* out_connected,
                           bool* out_scanning,
                           char* out_name,
                           size_t name_len,
                           char* out_mac,
                           size_t mac_len);
