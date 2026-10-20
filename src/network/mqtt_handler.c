#include "mqtt_handler.h"
#include "network_config.h"
#include <stdio.h>
#include <string.h>
#include "lwip/dns.h"
#include "lwip/apps/mqtt.h"
#include "lwip/sys.h"
#include "pico/cyw43_arch.h"

#define CONNECTION_RETRY_MS 5000
#define SUBSCRIPTION_RETRY_MS 1000

typedef struct {
    char topic[MQTT_TOPIC_MAX_LENGTH + 1];
    uint8_t qos;
    bool active;
} subscription_t;

static mqtt_client_t *client;
static struct mqtt_connect_client_info_t client_info;
static char will_topic[MQTT_TOPIC_MAX_LENGTH + 1];
static char will_payload[MQTT_PAYLOAD_MAX_LENGTH + 1];
static uint16_t keep_alive_seconds = 30;
static bool connection_pending;
static bool attempted;
static u32_t last_attempt_ms;
static subscription_t subscriptions[MQTT_MAX_SUBSCRIPTIONS];
static size_t subscription_count;
static bool subscription_pending;
static bool subscription_retry_wait;
static u32_t subscription_failed_ms;

// lwIP callback producer / Network Task consumer. All accesses use the lwIP lock.
static mqtt_message_t rx_queue[MQTT_RX_QUEUE_CAPACITY];
static size_t rx_head, rx_count;
static mqtt_message_t incoming;
static size_t incoming_used;
static bool incoming_valid;
static uint32_t dropped_messages;

//checks to see if this is a valid topic or filter. If filter is true, then the topic can contain wildcards.
static bool valid_topic(const char *topic, bool filter) {
    if (topic == NULL) return false;
    size_t length = strlen(topic);
    if (length == 0 || length > MQTT_TOPIC_MAX_LENGTH) return false;
    for (size_t i = 0; i < length; ++i) {
        if (topic[i] == '#' || topic[i] == '+') {
            if (!filter || (i > 0 && topic[i - 1] != '/')) return false;
            if (topic[i] == '#' && i + 1 != length) return false;
            if (topic[i] == '+' && i + 1 < length && topic[i + 1] != '/') return false;
        }
    }
    return true;
}

static void enqueue_incoming(void) {
    if (rx_count == MQTT_RX_QUEUE_CAPACITY) {
        ++dropped_messages;
    } else {
        rx_queue[(rx_head + rx_count) % MQTT_RX_QUEUE_CAPACITY] = incoming;
        ++rx_count;
    }
    incoming_valid = false;
}

// Callback for incoming publish messages. Called from lwIP's TCP thread.
static void incoming_publish_cb(void *arg, const char *topic, u32_t total_length) {
    (void)arg;
    if (incoming_valid) ++dropped_messages;
    incoming_valid = false;
    incoming_used = 0;
    if (!valid_topic(topic, false) || total_length > MQTT_PAYLOAD_MAX_LENGTH) {
        ++dropped_messages;
        return;
    }
    strcpy(incoming.topic, topic);
    incoming.length = total_length;
    incoming_valid = true;
    // An empty MQTT payload is valid, including retained-message clearing.
    if (total_length == 0) enqueue_incoming();
}

// Callback for incoming data. Called from lwIP's TCP thread.
static void incoming_data_cb(void *arg, const u8_t *data, u16_t length, u8_t flags) {
    (void)arg;
    if (!incoming_valid) return;
    if (length > incoming.length - incoming_used || (length && data == NULL)) {
        incoming_valid = false;
        ++dropped_messages;
        return;
    }
    if (length) memcpy(incoming.payload + incoming_used, data, length);
    incoming_used += length;
    if (flags & MQTT_DATA_FLAG_LAST) {
        if (incoming_used == incoming.length) {
            enqueue_incoming();
        } else {
            incoming_valid = false;
            ++dropped_messages;
        }
    }
}

// Callback for subscription status changes. Called from lwIP's TCP thread.
static void subscription_cb(void *arg, err_t result) {
    subscription_t *subscription = arg;
    subscription_pending = false;
    if (result == ERR_OK) {
        subscription->active = true;
        printf("Subscribed to %s\n", subscription->topic);
    } else {
        subscription_retry_wait = true;
        subscription_failed_ms = sys_now();
        printf("Subscription failed for %s: %d; will retry.\n", subscription->topic, result);
    }
}

// Service subscriptions, one at a time. This is called with the lwIP lock held.
static void service_subscriptions(void) {
    if (subscription_pending) return;
    if (subscription_retry_wait &&
        (u32_t)(sys_now() - subscription_failed_ms) < SUBSCRIPTION_RETRY_MS) return;
    subscription_retry_wait = false;
    // One request at a time avoids exhausting lwIP's pending request slots.
    for (size_t i = 0; i < subscription_count; ++i) {
        if (subscriptions[i].active) continue;
        subscription_pending = true;
        err_t result = mqtt_subscribe(client, subscriptions[i].topic, subscriptions[i].qos,
                                     subscription_cb, &subscriptions[i]);
        if (result != ERR_OK) {
            subscription_pending = false;
            subscription_retry_wait = true;
            subscription_failed_ms = sys_now();
            printf("Could not queue subscription: %d; will retry.\n", result);
        }
        return;
    }
}

// Callback for MQTT connection status changes. Called from lwIP's TCP thread.
static void mqtt_connection_cb(mqtt_client_t *mqtt, void *arg, mqtt_connection_status_t status) {
    (void)arg;
    connection_pending = false;
    subscription_pending = false;
    subscription_retry_wait = false;
    for (size_t i = 0; i < subscription_count; ++i) subscriptions[i].active = false;
    // Clean sessions: restore subscriptions and discard messages from an old session.
    dropped_messages += (uint32_t)rx_count + (incoming_valid ? 1u : 0u);
    incoming_valid = false;
    rx_head = rx_count = 0;
    if (status == MQTT_CONNECT_ACCEPTED) {
        printf("MQTT connected to %s.\n", MQTT_BROKER_HOST);
        mqtt_set_inpub_callback(mqtt, incoming_publish_cb, incoming_data_cb, NULL);
        service_subscriptions();
    } else {
        printf("MQTT disconnected/connection failed: %d; will retry.\n", status);
        last_attempt_ms = sys_now();
    }
}

// Connect after DNS resolves, either in the Network Task or DNS callback; locked.
static void connect_to_broker(const ip_addr_t *address, const char *client_id) {
    client_info.client_id = client_id;
    client_info.keep_alive = keep_alive_seconds;
    printf("Connecting to %s:%d as %s...\n", ipaddr_ntoa(address), MQTT_BROKER_PORT, client_id);
    err_t result = mqtt_client_connect(client, address, MQTT_BROKER_PORT,
                                      mqtt_connection_cb, NULL, &client_info);
    if (result != ERR_OK) {
        connection_pending = false;
        printf("Could not start MQTT connection: %d\n", result);
    }
}

// Callback for DNS resolution. Called from lwIP's TCP thread.
static void dns_found_cb(const char *name, const ip_addr_t *address, void *arg) {
    if (address != NULL) {
        connect_to_broker(address, (const char *)arg);
    } else {
        connection_pending = false;
        printf("DNS resolution failed for %s\n", name);
    }
}

// Service the MQTT connection, including DNS resolution and connection attempts. Called by the Network Task with the lwIP lock held.
static void service_connection(const char *client_id) {
    if (client && mqtt_client_is_connected(client)) {
        service_subscriptions();
        return;
    }
    if (connection_pending) return;
    if (attempted && (u32_t)(sys_now() - last_attempt_ms) < CONNECTION_RETRY_MS) return;
    attempted = true;
    last_attempt_ms = sys_now();
    if (client == NULL) client = mqtt_client_new();
    if (client == NULL) {
        printf("MQTT allocation failed; will retry.\n");
        return;
    }
    connection_pending = true;
    ip_addr_t address;
    err_t result = dns_gethostbyname(MQTT_BROKER_HOST, &address, dns_found_cb, (void *)client_id);
    if (result == ERR_OK) {
        connect_to_broker(&address, client_id);
    } else if (result != ERR_INPROGRESS) {
        connection_pending = false;
        printf("DNS request failed: %d\n", result);
    }
}

// Network Task entry point: take the lwIP lock before servicing the client.
void init_mqtt_client(const char *client_id) {
    if (client_id == NULL || client_id[0] == '\0') return;
    cyw43_arch_lwip_begin();
    service_connection(client_id);
    cyw43_arch_lwip_end();
}

// Save the broker's disconnect announcement before the first connection attempt.
// The public function acquires its own lwIP lock.
bool mqtt_configure_will(const char *topic, const char *payload, uint16_t keep_alive) {
    if (!valid_topic(topic, false) || payload == NULL ||
        strlen(payload) > MQTT_PAYLOAD_MAX_LENGTH || keep_alive == 0) return false;
    cyw43_arch_lwip_begin();
    bool accepted = !attempted;
    if (accepted) {
        strcpy(will_topic, topic);
        strcpy(will_payload, payload);
        keep_alive_seconds = keep_alive;
        client_info.will_topic = will_topic;
        client_info.will_msg = will_payload;
        client_info.will_qos = 1;
        client_info.will_retain = 1;
    }
    cyw43_arch_lwip_end();
    return accepted;
}

// Compatibility wrapper: register a filter at QoS 0.
bool mqtt_subscribe_topic(const char *topic) {
    return mqtt_subscribe_topic_qos(topic, 0);
}

// Copy a filter into the local registry. Broker acknowledgement happens later;
// registration success does not yet mean the subscription is active.
bool mqtt_subscribe_topic_qos(const char *topic, uint8_t qos) {
    if (!valid_topic(topic, true) || qos > 1) return false;
    cyw43_arch_lwip_begin();
    for (size_t i = 0; i < subscription_count; ++i) {
        if (strcmp(topic, subscriptions[i].topic) == 0) {
            bool same_qos = subscriptions[i].qos == qos;
            cyw43_arch_lwip_end();
            return same_qos;
        }
    }
    bool accepted = subscription_count < MQTT_MAX_SUBSCRIPTIONS;
    if (accepted) {
        strcpy(subscriptions[subscription_count].topic, topic);
        subscriptions[subscription_count].qos = qos;
        subscriptions[subscription_count++].active = false;
    }
    cyw43_arch_lwip_end();
    return accepted;
}

static void publish_cb(void *arg, err_t result) {
    (void)arg;
    if (result != ERR_OK) printf("Publish completion error: %d\n", result);
}

bool mqtt_send(const char *topic, const void *payload, size_t length) {
    return mqtt_send_ex(topic, payload, length, 0, false, NULL, NULL);
}

bool mqtt_send_ex(const char *topic, const void *payload, size_t length,
                  uint8_t qos, bool retain, mqtt_send_completion_t completion,
                  void *context) {
    if (!valid_topic(topic, false) || length > MQTT_PAYLOAD_MAX_LENGTH ||
        (length && payload == NULL) || qos > 1) return false;
    cyw43_arch_lwip_begin();
    bool queued = false;
    if (client && mqtt_client_is_connected(client)) {
        err_t result = mqtt_publish(client, topic, payload, (u16_t)length, qos, retain,
                                   completion ? completion : publish_cb, context);
        queued = result == ERR_OK;
        if (!queued) printf("Could not queue publish: %d\n", result);
    }
    cyw43_arch_lwip_end();
    return queued;
}

bool mqtt_receive(mqtt_message_t *message) {
    if (message == NULL) return false;
    cyw43_arch_lwip_begin();
    bool available = rx_count > 0;
    if (available) {
        *message = rx_queue[rx_head];
        rx_head = (rx_head + 1) % MQTT_RX_QUEUE_CAPACITY;
        --rx_count;
    }
    cyw43_arch_lwip_end();
    return available;
}

uint32_t mqtt_received_drop_count(void) {
    cyw43_arch_lwip_begin();
    uint32_t count = dropped_messages;
    cyw43_arch_lwip_end();
    return count;
}

bool is_mqtt_connected(void) {
    cyw43_arch_lwip_begin();
    bool connected = client && mqtt_client_is_connected(client);
    cyw43_arch_lwip_end();
    return connected;
}

bool mqtt_is_ready(void) {
    cyw43_arch_lwip_begin();
    bool ready = client && mqtt_client_is_connected(client);
    for (size_t i = 0; i < subscription_count; ++i) ready = ready && subscriptions[i].active;
    cyw43_arch_lwip_end();
    return ready;
}
