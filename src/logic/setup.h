#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "node_config.h"

#define SETUP_COORDINATOR_NODE 1
#define IS_COORDINATOR (identity_node_id() == SETUP_COORDINATOR_NODE)

#ifndef MQTT_ORDER_TOPIC
#define MQTT_ORDER_TOPIC MQTT_TOPIC_ROOT "/game/order"
#endif
#ifndef MQTT_SETUP_TOPIC
#define MQTT_SETUP_TOPIC MQTT_TOPIC_ROOT "/game/setup"
#endif

enum { SETUP_OP_WRONG = 1, SETUP_OP_START = 2 };

bool setup_start(void);                         // creates queues (+ coordinator task)
bool setup_register_subscriptions(void);        // called by net_task once
bool setup_handle_message(const char *topic, const uint8_t *p, size_t len); // true = consumed
void setup_publish_output(void);                // called by net_task (coordinator only)