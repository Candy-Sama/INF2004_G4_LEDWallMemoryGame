#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

bool wifi_init_and_connect(void);
bool is_wifi_connected();

#endif