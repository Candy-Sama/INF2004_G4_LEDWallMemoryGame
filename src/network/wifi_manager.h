#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

// Blocking startup connection (up to 30 seconds), using WPA2 credentials.
// Caller owns the strings and must keep them valid until this call returns.
// Credentials are not stored or logged. No live network switching/provisioning.
bool wifi_init_and_connect(const char *ssid, const char *password);
bool is_wifi_connected(void);

#endif
