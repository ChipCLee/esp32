#pragma once

#include "types.h"

void web_server_init();
void web_server_start();
void web_server_stop();
bool web_server_is_active();
void web_server_task(void* param);
