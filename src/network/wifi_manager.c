#include "wifi_manager.h"
#include "wifi_creds.h"
#include "pico/cyw43_arch.h"
#include <stdio.h>

bool wifi_init_and_connect(void) {
    if (cyw43_arch_init()) {
        printf("Wi-Fi init failed\n");
        return false;
    }

    cyw43_arch_enable_sta_mode();
    printf("Connecting to Wi-Fi...\n");
    
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("Failed to connect to Wi-Fi.\n");
        return false;
    } 
    
    printf("Wi-Fi Connected.\n");
    return true;
}

bool is_wifi_connected() {
    return cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP;
}