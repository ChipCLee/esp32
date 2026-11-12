#pragma once

#include "types.h"
#include <SPI.h>

void sd_logger_init(SPIClass* shared_spi);
void sd_logger_enqueue(const discovered_device_t& dev);
void sd_logger_task(void* param);
bool sd_logger_is_ready();

// Generate CSV string of all current devices (or read from SD card if available)
String sd_logger_export_csv();

