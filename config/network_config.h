#ifndef NETWORK_CONFIG_H
#define NETWORK_CONFIG_H

// Local broker DNS name. Replace with your broker PC's LAN IPv4 address if your
// router does not resolve this name; localhost would refer to the Pico itself.
#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST "10.152.207.137"
#endif
#define MQTT_BROKER_PORT 1883

#endif
