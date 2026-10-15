#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

// Change this to the number of the board you are about to flash.
#define NODE_ID 1

// 1 enables the one-shot demo on node 1 only; 0 disables it on all nodes.
#define ENABLE_PEER_TEST 1
#define PEER_NODE_ID 2

#if NODE_ID < 1 || NODE_ID > 6
#error "NODE_ID must be between 1 and 6"
#endif

// Two steps expand a number macro before converting it to text.
#define NODE_STRING_IMPL(value) #value
#define NODE_STRING(value) NODE_STRING_IMPL(value)

#define MQTT_STATUS_TOPIC "se4/memorygame/node" NODE_STRING(NODE_ID)
#define MQTT_COMMAND_TOPIC MQTT_STATUS_TOPIC "/command"
#define MQTT_STATUS_MESSAGE "Hello from my Pico " NODE_STRING(NODE_ID) "!"
#define MQTT_PEER_COMMAND_TOPIC "se4/memorygame/node" NODE_STRING(PEER_NODE_ID) "/command"

#endif
