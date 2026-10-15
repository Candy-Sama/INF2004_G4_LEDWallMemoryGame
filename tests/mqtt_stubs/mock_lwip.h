#ifndef MOCK_LWIP_H
#define MOCK_LWIP_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef int err_t;
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_INPROGRESS -5
#define MQTT_DATA_FLAG_LAST 1
typedef int mqtt_connection_status_t;
#define MQTT_CONNECT_ACCEPTED 0
#define MQTT_CONNECT_DISCONNECTED 256
typedef struct { int connected; } mqtt_client_t;
typedef struct { int unused; } ip_addr_t;
struct mqtt_connect_client_info_t { const char *client_id; u16_t keep_alive; };
typedef void (*request_cb_t)(void *, err_t);
typedef void (*connection_cb_t)(mqtt_client_t *, void *, mqtt_connection_status_t);
typedef void (*publish_cb_t)(void *, const char *, u32_t);
typedef void (*data_cb_t)(void *, const u8_t *, u16_t, u8_t);
void cyw43_arch_lwip_begin(void);
void cyw43_arch_lwip_end(void);
u32_t sys_now(void);
mqtt_client_t *mqtt_client_new(void);
int mqtt_client_is_connected(mqtt_client_t *);
err_t mqtt_client_connect(mqtt_client_t *, const ip_addr_t *, u16_t, connection_cb_t, void *, const struct mqtt_connect_client_info_t *);
void mqtt_set_inpub_callback(mqtt_client_t *, publish_cb_t, data_cb_t, void *);
err_t mqtt_subscribe(mqtt_client_t *, const char *, u8_t, request_cb_t, void *);
err_t mqtt_publish(mqtt_client_t *, const char *, const void *, u16_t, u8_t, u8_t, request_cb_t, void *);
err_t dns_gethostbyname(const char *, ip_addr_t *, void (*)(const char *, const ip_addr_t *, void *), void *);
const char *ipaddr_ntoa(const ip_addr_t *);
#endif
