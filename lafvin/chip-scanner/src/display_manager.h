#pragma once

#include "types.h"
#include <SPI.h>

typedef enum : uint8_t {
    DISPLAY_MODE_DASHBOARD = 0, // Split Dashboard HUD (Status + Protocol Matrix + 6 Logs)
    DISPLAY_MODE_TERMINAL  = 1  // Full-Screen Activity Log Terminal (14 Lines)
} display_mode_t;

void display_manager_init(SPIClass* shared_spi);
void display_manager_task(void* param);
void display_manager_set_backlight(bool on);
bool display_manager_get_backlight();
display_mode_t display_manager_get_mode();
void display_manager_toggle_mode();

