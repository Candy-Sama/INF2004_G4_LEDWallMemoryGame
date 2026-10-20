#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "ws2812.pio.h"
#include "FreeRTOSConfig.h"
#include "wifi_creds.h"
#include "node_config.h"
#include "ledmatrix.h"
#include "imudriver.h"
#include "network_task.h"

#define TAP_QUEUE_LENGTH 10
#define APPLICATION_PRIORITY 2
#define APPLICATION_STACK_WORDS 1024
#define APPLICATION_INTERVAL_MS 100

QueueHandle_t tapEventQueue;

static const network_task_config_t network_config = {
    .ssid = WIFI_SSID,
    .password = WIFI_PASSWORD,
    .client_id = "SE4_Pico_Node_" NODE_STRING(NODE_ID),
    .subscription = MQTT_COMMAND_TOPIC,
    .status_topic = MQTT_PRESENCE_TOPIC,
};

static void application_task(void *arg) {
    (void)arg;
    led_init();

    for (;;) {
        for (int i = 0; i < NUM_LEDS; ++i) {
            put_pixel(urgb_u32(5, 0, 5));
        }

        CompactTapData tap;
        if (xQueueReceive(tapEventQueue, &tap, 0) == pdPASS &&
            tap.tap_event && !tap.imu_fault) {
            static const char payload[] = "TAP DETECTED";
            if (!network_publish(MQTT_TAP_TOPIC, payload, sizeof(payload) - 1,
                                 1, false)) {
                printf("Could not queue MQTT tap publication.\n");
            }
        }

        mqtt_message_t incoming;
        while (network_receive(&incoming, 0)) {
            printf("Received unhandled MQTT message on %s (%lu bytes).\n",
                   incoming.topic, (unsigned long)incoming.length);
        }

        vTaskDelay(pdMS_TO_TICKS(APPLICATION_INTERVAL_MS));
    }
}

int main(void) {
    stdio_init_all();
    printf("\n--- Pico W boot ---\n");

    tapEventQueue = xQueueCreate(TAP_QUEUE_LENGTH, sizeof(CompactTapData));
    if (tapEventQueue == NULL) {
        printf("Could not create tap event queue.\n");
        return 1;
    }

    if (!network_task_start(&network_config)) {
        printf("Could not start network task.\n");
        return 1;
    }

    imu_init(NODE_ID);

    if (xTaskCreate(application_task, "Application", APPLICATION_STACK_WORDS,
                    NULL, APPLICATION_PRIORITY, NULL) != pdPASS) {
        printf("Could not create application task.\n");
        return 1;
    }

    printf("Starting FreeRTOS scheduler.\n");
    vTaskStartScheduler();

    printf("FreeRTOS scheduler returned unexpectedly.\n");
    return 1;
}
