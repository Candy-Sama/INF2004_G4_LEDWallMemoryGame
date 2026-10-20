#include "wifi_setup.h"
#include "wifi_manager.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

#define SSID_MAX_BYTES 32
#define PASSWORD_MAX_BYTES 63

static void clear_secret(char *buffer, size_t size) {
    volatile char *bytes = buffer;
    while (size--) *bytes++ = 0;
}

// Accept LF, CR, or CRLF. Never silently truncate an oversized credential.
// Password characters are never echoed by the firmware.
static bool read_line(char *buffer, size_t capacity, bool echo) {
    static bool skip_lf;
    size_t used = 0;
    bool invalid = false;
    while (true) {
        if (!stdio_usb_connected()) {
            clear_secret(buffer, capacity);
            return false;
        }
        int ch = getchar_timeout_us(100000);
        if (ch == PICO_ERROR_TIMEOUT) continue;
        if (ch < 0) continue;
        if (skip_lf) {
            skip_lf = false;
            if (ch == '\n') continue;
        }
        if (ch == '\r' || ch == '\n') {
            skip_lf = ch == '\r';
            buffer[used] = '\0';
            printf("\n");
            return !invalid;
        }
        if (ch == '\b' || ch == 127) {
            if (used && !invalid) {
                buffer[--used] = '\0';
                if (echo) printf("\b \b");
            }
            continue;
        }
        if (ch < 32 || ch > 255 || used + 1 >= capacity) {
            invalid = true;
            continue;
        }
        if (!invalid) {
            buffer[used++] = (char)ch;
            if (echo) putchar(ch);
        }
    }
}

void wifi_setup_prompt(void) {
    char ssid[SSID_MAX_BYTES + 1] = {0};
    char password[PASSWORD_MAX_BYTES + 1] = {0};
    while (true) {
        while (!stdio_usb_connected()) sleep_ms(100);
        printf("\nWi-Fi setup (WPA2; enter credentials after every restart).\n");
        printf("Send each entry with Enter / a newline.\nWi-Fi name (SSID): ");
        fflush(stdout);
        bool ssid_valid = read_line(ssid, sizeof(ssid), true);
        if (!ssid_valid || ssid[0] == '\0') {
            printf("Enter a network name of 1-32 bytes; no control characters.\n");
            clear_secret(ssid, sizeof(ssid));
            continue;
        }
        printf("Wi-Fi password (8-63 characters; input hidden): ");
        fflush(stdout);
        bool password_valid = read_line(password, sizeof(password), false);
        if (!password_valid || strlen(password) < 8) {
            printf("Enter a WPA2 passphrase of 8-63 characters; no control characters.\n");
            clear_secret(ssid, sizeof(ssid));
            clear_secret(password, sizeof(password));
            continue;
        }
        bool connected = wifi_init_and_connect(ssid, password);
        clear_secret(ssid, sizeof(ssid));
        clear_secret(password, sizeof(password));
        if (connected) return;
        printf("Connection failed. Check the network and re-enter credentials.\n");
    }
}
