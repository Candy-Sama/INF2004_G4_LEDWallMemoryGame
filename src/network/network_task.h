#ifndef NETWORK_TASK_H
#define NETWORK_TASK_H
#include "FreeRTOS.h"
#include "queue.h"
#include "mqtt_handler.h"

// Storage and strings must remain valid for the task lifetime. Application owns
// topics/payload meanings. Start once before vTaskStartScheduler().
typedef struct {
    const char *ssid, *password, *client_id;
    const char *subscription, *status_topic;
} network_task_config_t;
bool network_task_start(const network_task_config_t *config);

// Task-context APIs, not ISR APIs. Copies bytes. false: offline, invalid, or full.
// true: queued to owner only, NOT broker delivery. Offline requests are discarded.
bool network_publish(const char *topic, const void *payload, size_t length,
                     uint8_t qos, bool retain);
// Exactly one application dispatcher consumes this queue; routes to teammates.
bool network_receive(mqtt_message_t *message, TickType_t wait);
bool network_ready(void); // Transport readiness, not game synchronisation.
uint32_t network_drop_count(void); // TX failures and application RX overflow.
#endif
