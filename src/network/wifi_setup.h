#ifndef WIFI_SETUP_H
#define WIFI_SETUP_H

// Prompt over USB serial until WPA2 connection succeeds. Credentials are kept
// only for the connection attempt; no flash storage or hardcoded fallback.
void wifi_setup_prompt(void);

#endif
