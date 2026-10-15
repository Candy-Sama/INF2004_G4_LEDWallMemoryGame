#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MQTT_TOPIC_MAX_LENGTH 95
#define MQTT_PAYLOAD_MAX_LENGTH 256
#define MQTT_RX_QUEUE_CAPACITY 8
#define MQTT_MAX_SUBSCRIPTIONS 8

// Topic is terminated text; payload is binary data, NOT a C string.
typedef struct {
    char topic[MQTT_TOPIC_MAX_LENGTH + 1];
    uint8_t payload[MQTT_PAYLOAD_MAX_LENGTH];
    size_t length;
} mqtt_message_t;

// Call these APIs from the main loop after Wi-Fi architecture initialisation.
// They handle network locking internally. The supplied client_id must stay valid.
// Call repeatedly while Wi-Fi is up to service connection/subscription retries.
void init_mqtt_client(const char *client_id);

// Copies a nonempty topic filter. Supports MQTT +/# wildcards. Idempotent.
// true means registered locally, not yet acknowledged by the broker.
// Filters are retained across reconnects; there is no removal API yet.
bool mqtt_subscribe_topic(const char *topic);
bool mqtt_is_ready(void);  // Connected AND all registered subscriptions accepted.
bool is_mqtt_connected(void);

// QoS 0, retain=false; true means queued locally, NOT delivery confirmation.
// Rejects wildcards, oversized inputs and disconnected sends. No offline queue.
// NULL payload is allowed only when length == 0.
bool mqtt_send(const char *topic, const void *payload, size_t length);

// Pops the oldest complete message. false means queue empty or NULL destination.
// Queue full: discard the newest arriving message. Oversized/malformed messages
// are discarded whole. Disconnect clears queued/partial messages to avoid stale
// game events. The discard counter includes these losses and is cumulative.
bool mqtt_receive(mqtt_message_t *message);
uint32_t mqtt_received_drop_count(void);

#endif
