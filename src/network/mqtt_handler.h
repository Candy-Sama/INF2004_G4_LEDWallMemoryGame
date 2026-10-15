#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <stdbool.h>

#include "node_config.h"

typedef enum {
    LED_COMMAND_NONE,
    LED_COMMAND_RED,
    LED_COMMAND_GREEN,
    LED_COMMAND_OFF
} led_command_t;

// Consume the latest received colour command safely from the main loop.
led_command_t mqtt_take_led_command(void);

// Call from the main loop; network locking is handled inside this module.
// client_id must remain valid for the client lifetime.
// Repeated calls retry failed attempts with a five-second minimum interval.
void init_mqtt_client(const char* client_id);
// Returns true when queued locally, not when a receiver has applied the message.
bool publish_message(const char* topic, const char* payload);

bool is_mqtt_connected(void);

#endif
