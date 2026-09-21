#include "mqtt_handler.h"
#include <stdio.h>
#include <string.h> 
#include "lwip/dns.h" // Added DNS library

mqtt_client_t *mqtt_client;
static struct mqtt_connect_client_info_t ci = {0};

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    if (status == MQTT_CONNECT_ACCEPTED) {
        printf("Successfully connected to HiveMQ Public Broker!\n");
    } else {
        printf("MQTT Connection failed with status: %d\n", status);
    }
}

// Function to trigger once the IP is found
static void connect_to_broker(const ip_addr_t *ipaddr, const char* client_id) {
    ci.client_id = client_id; 
    err_t err = mqtt_client_connect(mqtt_client, ipaddr, 1883, mqtt_connection_cb, NULL, &ci);
    if (err != ERR_OK) {
        printf("Failed to initiate MQTT connection\n");
    }
}

// DNS Callback: Fires when the router returns the IP address
static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *callback_arg) {
    const char* client_id = (const char*)callback_arg;
    if (ipaddr != NULL) {
        printf("DNS resolved %s to IP: %s\n", name, ip4addr_ntoa(ipaddr));
        connect_to_broker(ipaddr, client_id);
    } else {
        printf("DNS resolution failed for %s\n", name);
    }
}

void init_mqtt_client(const char* client_id) {
    mqtt_client = mqtt_client_new();
    ip_addr_t broker_ip;
    
    printf("Resolving MQTT broker domain...\n");
    
    // Request the IP address for broker.hivemq.com
    err_t err = dns_gethostbyname("broker.hivemq.com", &broker_ip, dns_found_cb, (void*)client_id);
    
    if (err == ERR_OK) {
        // The IP was already in the Pico's cache, connect immediately
        connect_to_broker(&broker_ip, client_id);
    } else if (err != ERR_INPROGRESS) {
        printf("DNS request failed with error: %d\n", err);
    }
}

static void mqtt_pub_request_cb(void *arg, err_t result) {
    if(result != ERR_OK) {
        printf("Publish failed: %d\n", result);
    }
}

void publish_message(const char* topic, const char* payload) {
    if (mqtt_client != NULL && mqtt_client_is_connected(mqtt_client)) {
        err_t err = mqtt_publish(mqtt_client, topic, payload, strlen(payload), 0, 0, mqtt_pub_request_cb, NULL);
        if(err != ERR_OK) {
            printf("Publish error: %d\n", err);
        }
    }
}