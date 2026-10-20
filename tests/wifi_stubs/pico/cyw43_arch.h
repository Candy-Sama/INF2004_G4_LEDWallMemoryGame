#ifndef WIFI_TEST_STUB_H
#define WIFI_TEST_STUB_H
#define CYW43_AUTH_WPA2_AES_PSK 1
#define CYW43_ITF_STA 0
#define CYW43_LINK_UP 3
extern int cyw43_state;
int cyw43_arch_init(void);
void cyw43_arch_enable_sta_mode(void);
int cyw43_arch_wifi_connect_timeout_ms(const char *, const char *, unsigned, unsigned);
int cyw43_tcpip_link_status(int *, int);
#endif
