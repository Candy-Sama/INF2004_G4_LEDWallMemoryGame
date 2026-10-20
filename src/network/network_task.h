#ifndef NETWORK_TASK_H
#define NETWORK_TASK_H
#include "FreeRTOS.h"
#include "mqtt_handler.h"

// Storage and strings must remain valid for the task lifetime. Application owns
// topics/payload meanings. Start once before vTaskStartScheduler().
typedef struct {
    const char *ssid, *password, *client_id;
    const char *subscription, *status_topic;
} network_task_config_t;
bool network_task_start(const network_task_config_t *config);

// Task-context APIs, not ISR APIs. Copies topic/payload before returning, so the
// caller may reuse its buffers. Does not wait for queue space.
// false: offline, invalid, or full. true: queued, NOT broker delivery.
// qos is 0/1. Retain state/assignments, not taps. Offline requests are discarded.
bool network_publish(const char *topic, const void *payload, size_t length,
                     uint8_t qos, bool retain);
// Copy the oldest received message into *message. The caller passes &message
// so the function can fill that variable. Payload bytes are NOT NUL-terminated.
// wait_ticks: maximum wait if empty. 0 = return immediately; use
// pdMS_TO_TICKS(20) for about 20 ms, or portMAX_DELAY for an indefinite wait
// in this configuration. Existing queued data returns immediately.
// true = copied a message; false = timeout, NULL destination, or not started.
// Exactly one application dispatcher consumes this queue; routes to teammates.
bool network_receive(mqtt_message_t *message, TickType_t wait_ticks);
bool network_ready(void); // Transport readiness, not game synchronisation.
uint32_t network_drop_count(void); // TX failures and application RX overflow.
#endif
