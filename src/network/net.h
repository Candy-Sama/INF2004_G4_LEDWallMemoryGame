// net.h
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_MAX_HANDLERS    8
#define NET_PUB_MAX_PAYLOAD 32

// Called inside net_task when a message matches the filter.
// Must be tiny and must never block (use xQueueSend with timeout 0).
typedef void (*net_rx_handler_t)(const char *topic, const uint8_t *payload, size_t len);

// Call before net_start(). Supports MQTT + and # wildcards. The filter is copied.
bool net_register_handler(const char *topic_filter, net_rx_handler_t fn);

// Queue a message (QoS 1). Copies topic and data. Safe from any task, never blocks.
// true = queued, not delivered.
bool net_publish(const char *topic, const void *data, size_t len, bool retain);

//Arrival time (time_us_32) of the message a handler is being called for. 
//Valid only inside the net_rx_handler_t call. Stamped inside the lwIP callback, not at the 10 ms poll.
uint32_t net_rx_time_us(void);

bool net_start(void);   // call last, after every module has registered its handlers