#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MQTT_TOPIC_MAX_LENGTH 95
#define MQTT_PAYLOAD_MAX_LENGTH 256
#define MQTT_RX_QUEUE_CAPACITY 8
#define MQTT_MAX_SUBSCRIPTIONS 8

// Completion runs in lwIP callback context: only record a result or notify the
// owning task. Do not call these APIs, block, or render LEDs from this callback.
// result == 0 means success (QoS 1: broker PUBACK, not panel application ACK).
typedef void (*mqtt_send_completion_t)(void *context, int8_t result);

// Configure before the first init_mqtt_client call. Copies topic and text payload;
// retained QoS 1 will is reused on reconnect. Keepalive must be nonzero seconds.
bool mqtt_configure_will(const char *topic, const char *payload, uint16_t keep_alive);

// Topic is terminated text; payload is binary data, NOT a C string.
typedef struct {
    char topic[MQTT_TOPIC_MAX_LENGTH + 1];
    uint8_t payload[MQTT_PAYLOAD_MAX_LENGTH];
    size_t length;
} mqtt_message_t;

// Low-level APIs: only the Network Task calls these after Wi-Fi architecture
// initialisation. Other tasks use network_task.h and its copied queues.
// They handle network locking internally. The supplied client_id must stay valid.
// Call repeatedly while Wi-Fi is up to service connection/subscription retries.
void init_mqtt_client(const char *client_id);

// Copies a nonempty topic filter. Supports MQTT +/# wildcards. Idempotent.
// true means registered locally, not yet acknowledged by the broker.
// Filters are retained across reconnects; there is no removal API yet.
bool mqtt_subscribe_topic(const char *topic);
// Supports QoS 0/1. Re-registering with a different QoS is rejected.
bool mqtt_subscribe_topic_qos(const char *topic, uint8_t qos);
// Changes after every accepted connection, even if the task missed the outage.
uint32_t mqtt_connection_generation(void);
bool mqtt_is_ready(void);  // Connected AND all registered subscriptions accepted.
bool is_mqtt_connected(void);

// QoS 0, retain=false; true means queued locally, NOT delivery confirmation.
// Rejects wildcards, oversized inputs and disconnected sends. No offline queue.
// NULL payload is allowed only when length == 0.
bool mqtt_send(const char *topic, const void *payload, size_t length);
// callback/context must remain valid until completion or connection loss.
// No callback is promised on disconnect; application resync resolves uncertainty.
bool mqtt_send_ex(const char *topic, const void *payload, size_t length,
                  uint8_t qos, bool retain, mqtt_send_completion_t completion,
                  void *context);

// Pops the oldest complete message. false means queue empty or NULL destination.
// Queue full: discard the newest arriving message. Oversized/malformed messages
// are discarded whole. Disconnect clears queued/partial messages to avoid stale
// game events. The discard counter includes these losses and is cumulative.
bool mqtt_receive(mqtt_message_t *message);
uint32_t mqtt_received_drop_count(void);

#endif
