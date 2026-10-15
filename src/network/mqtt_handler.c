#include "mqtt_handler.h"
#include <stdio.h>
#include <string.h> 
#include "lwip/dns.h"
#include "lwip/apps/mqtt.h"
#include "lwip/sys.h"
#include "pico/cyw43_arch.h"

static mqtt_client_t *mqtt_client;
static struct mqtt_connect_client_info_t ci = {0};
static bool connection_pending;
static bool attempted;
static u32_t last_attempt_ms;
static led_command_t pending_command;
static char incoming_command[16];
static u32_t incoming_expected;
static size_t incoming_used;
static bool incoming_valid;

// Incoming payloads can arrive in fragments and are not null-terminated.
static void incoming_publish_cb(void *arg, const char *topic, u32_t total_len) {
    incoming_used = 0;
    incoming_expected = total_len;
    incoming_valid = strcmp(topic, MQTT_COMMAND_TOPIC) == 0 &&
                     total_len > 0 && total_len < sizeof(incoming_command);
}

static void incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    if (!incoming_valid) {
        return;
    }
    if (incoming_used + len > incoming_expected) {
        incoming_valid = false;
        return;
    }
    memcpy(incoming_command + incoming_used, data, len);
    incoming_used += len;
    if (flags & MQTT_DATA_FLAG_LAST) {
        incoming_valid = false;
        if (incoming_used != incoming_expected) {
            return;
        }
        // Compare bytes as well as length so embedded nulls cannot match.
        led_command_t command = LED_COMMAND_NONE;
        if (incoming_used == 3 && memcmp(incoming_command, "RED", 3) == 0) {
            command = LED_COMMAND_RED;
        } else if (incoming_used == 5 && memcmp(incoming_command, "GREEN", 5) == 0) {
            command = LED_COMMAND_GREEN;
        } else if (incoming_used == 3 && memcmp(incoming_command, "OFF", 3) == 0) {
            command = LED_COMMAND_OFF;
        }
        if (command != LED_COMMAND_NONE) {
            // This colour demo keeps the latest command if several arrive together.
            pending_command = command;
        } else {
            printf("Unknown LED command. Use RED, GREEN or OFF (uppercase).\n");
        }
    }
}

static void subscription_cb(void *arg, err_t result) {
    if (result == ERR_OK) {
        printf("Subscribed to %s. Ready for RED, GREEN or OFF.\n", MQTT_COMMAND_TOPIC);
    } else {
        printf("Command subscription failed: %d; reconnecting.\n", result);
        mqtt_disconnect(mqtt_client);
        last_attempt_ms = sys_now();
    }
}

led_command_t mqtt_take_led_command(void) {
    cyw43_arch_lwip_begin();
    led_command_t command = pending_command;
    pending_command = LED_COMMAND_NONE;
    cyw43_arch_lwip_end();
    return command;
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    connection_pending = false;
    incoming_valid = false;
    pending_command = LED_COMMAND_NONE;
    if (status == MQTT_CONNECT_ACCEPTED) {
        printf("MQTT connected to %s.\n", MQTT_BROKER_HOST);
        mqtt_set_inpub_callback(client, incoming_publish_cb, incoming_data_cb, NULL);
        // Subscribe again after every reconnect because sessions are clean.
        err_t err = mqtt_subscribe(client, MQTT_COMMAND_TOPIC, 0, subscription_cb, NULL);
        if (err != ERR_OK) {
            printf("Could not start command subscription: %d; reconnecting.\n", err);
            mqtt_disconnect(client);
            last_attempt_ms = sys_now();
        }
    } else {
        printf("MQTT Connection failed with status: %d\n", status);
        if (status == MQTT_CONNECT_DISCONNECTED) {
            printf("Broker connection closed or TCP failed. Will retry automatically.\n");
        } else if (status == MQTT_CONNECT_TIMEOUT) {
            printf("MQTT handshake/keepalive timed out. Will retry automatically.\n");
        }
        last_attempt_ms = sys_now();
    }
}

// Function to trigger once the IP is found
static void connect_to_broker(const ip_addr_t *ipaddr, const char* client_id) {
    ci.client_id = client_id; 
    ci.keep_alive = 30;
    printf("Connecting to %s:%d as %s...\n", ipaddr_ntoa(ipaddr), MQTT_BROKER_PORT, client_id);
    err_t err = mqtt_client_connect(mqtt_client, ipaddr, MQTT_BROKER_PORT, mqtt_connection_cb, NULL, &ci);
    if (err != ERR_OK) {
        connection_pending = false;
        printf("Failed to initiate MQTT connection, lwIP error: %d\n", err);
    }
}

// DNS Callback: Fires when the router returns the IP address
static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *callback_arg) {
    const char* client_id = (const char*)callback_arg;
    if (ipaddr != NULL) {
        printf("DNS resolved %s to IP: %s\n", name, ip4addr_ntoa(ipaddr));
        connect_to_broker(ipaddr, client_id);
    } else {
        connection_pending = false;
        printf("DNS resolution failed for %s\n", name);
    }
}

static void start_connection(const char* client_id) {
    // Called under the lwIP lock. Allow only one DNS/connect attempt at a time.
    if (connection_pending || (mqtt_client && mqtt_client_is_connected(mqtt_client))) {
        return;
    }
    if (attempted && (u32_t)(sys_now() - last_attempt_ms) < 5000) {
        return;
    }
    attempted = true;
    last_attempt_ms = sys_now();
    if (mqtt_client == NULL) {
        mqtt_client = mqtt_client_new();
        if (mqtt_client == NULL) {
            printf("MQTT client allocation failed; retrying later.\n");
            return;
        }
    }
    connection_pending = true;
    ip_addr_t broker_ip;
    
    printf("Resolving MQTT broker domain...\n");
    
    err_t err = dns_gethostbyname(MQTT_BROKER_HOST, &broker_ip, dns_found_cb, (void*)client_id);
    
    if (err == ERR_OK) {
        // The IP was already in the Pico's cache, connect immediately
        connect_to_broker(&broker_ip, client_id);
    } else if (err != ERR_INPROGRESS) {
        connection_pending = false;
        printf("DNS request failed with error: %d\n", err);
    }
}

void init_mqtt_client(const char* client_id) {
    cyw43_arch_lwip_begin();
    start_connection(client_id);
    cyw43_arch_lwip_end();
}

static void mqtt_pub_request_cb(void *arg, err_t result) {
    if(result != ERR_OK) {
        printf("Publish failed: %d\n", result);
    }
}

bool publish_message(const char* topic, const char* payload) {
    bool queued = false;
    cyw43_arch_lwip_begin();
    if (mqtt_client != NULL && mqtt_client_is_connected(mqtt_client)) {
        err_t err = mqtt_publish(mqtt_client, topic, payload, strlen(payload), 0, 0, mqtt_pub_request_cb, NULL);
        if(err != ERR_OK) {
            printf("Publish error: %d\n", err);
        }
        queued = err == ERR_OK;
    }
    cyw43_arch_lwip_end();
    return queued;
}

bool is_mqtt_connected(void) {
    cyw43_arch_lwip_begin();
    bool connected = (mqtt_client != NULL && mqtt_client_is_connected(mqtt_client));
    cyw43_arch_lwip_end();
    return connected;
}
