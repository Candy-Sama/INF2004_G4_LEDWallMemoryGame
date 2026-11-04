#pragma once
#include <stdbool.h>
#include "node_config.h"
#include "node_identity.h"

#define SETUP_COORDINATOR_NODE 1
#define IS_COORDINATOR (identity_node_id() == SETUP_COORDINATOR_NODE)

#ifndef MQTT_ORDER_TOPIC
#define MQTT_ORDER_TOPIC MQTT_TOPIC_ROOT "/game/order"
#endif
#ifndef MQTT_SETUP_TOPIC
#define MQTT_SETUP_TOPIC MQTT_TOPIC_ROOT "/game/setup"
#endif

enum {
    SETUP_OP_WRONG   = 1,
    SETUP_OP_START   = 2,
    SETUP_OP_ARRANGE = 3,
    SETUP_OP_SLOT    = 4
};

bool setup_start(void);   // registers its own net handlers