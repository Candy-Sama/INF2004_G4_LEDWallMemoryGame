#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wifi_manager.h"
#include "pico/cyw43_arch.h"

int cyw43_state;
static int init_calls, connect_calls, init_result, connect_result;
static const char *expected_ssid, *expected_password;
int cyw43_arch_init(void) { ++init_calls; return init_result; }
void cyw43_arch_enable_sta_mode(void) {}
int cyw43_arch_wifi_connect_timeout_ms(const char *ssid, const char *password,
                                     unsigned auth, unsigned timeout) {
    ++connect_calls;
    assert(strcmp(ssid, expected_ssid) == 0);
    assert(strcmp(password, expected_password) == 0);
    assert(auth == CYW43_AUTH_WPA2_AES_PSK && timeout == 30000);
    return connect_result;
}
int cyw43_tcpip_link_status(int *state, int interface) {
    assert(state == &cyw43_state && interface == CYW43_ITF_STA);
    return CYW43_LINK_UP;
}
int main(void) {
    assert(!wifi_init_and_connect(NULL, "test-only"));
    assert(!wifi_init_and_connect("", "test-only"));
    assert(!wifi_init_and_connect("demo", NULL));
    assert(!wifi_init_and_connect("demo", ""));
    assert(init_calls == 0 && connect_calls == 0);
    expected_ssid = "test-network-a";
    expected_password = "test-only-a";
    init_result = -1;
    assert(!wifi_init_and_connect(expected_ssid, expected_password));
    assert(init_calls == 1 && connect_calls == 0);
    init_result = 0;
    connect_result = -1;
    assert(!wifi_init_and_connect(expected_ssid, expected_password));
    assert(init_calls == 2 && connect_calls == 1);
    // A configuration module supplies different values after the failed attempt.
    expected_ssid = "test-network-b";
    expected_password = "test-only-b";
    connect_result = 0;
    assert(wifi_init_and_connect(expected_ssid, expected_password));
    assert(init_calls == 2 && connect_calls == 2);
    assert(is_wifi_connected());
    puts("Wi-Fi parameter tests passed.");
}
