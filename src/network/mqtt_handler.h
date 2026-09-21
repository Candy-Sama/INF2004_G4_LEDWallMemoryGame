#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include "lwip/apps/mqtt.h"
#include "lwip/ip4_addr.h"

void init_mqtt_client(const char* client_id);
void publish_message(const char* topic, const char* payload);

#endif