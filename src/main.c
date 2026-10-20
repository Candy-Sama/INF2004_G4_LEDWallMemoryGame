#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"         // Added for your Mutex
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "ws2812.pio.h"
#include "FreeRTOSConfig.h"
#include "wifi_creds.h"
#include "node_config.h"
#include "ledmatrix.h"
#include "imudriver.h"
#include "network_task.h"
#include "game_logic.h"     // Added for Master Logic

#define TAP_QUEUE_LENGTH 10
#define APPLICATION_PRIORITY 2
#define APPLICATION_STACK_WORDS 1024
#define APPLICATION_INTERVAL_MS 10

QueueHandle_t tapEventQueue;

// --- MASTER NODE GLOBALS ---
SemaphoreHandle_t game_state_mutex;
GameManager master_game;

// Network config: Master node listens to ALL tap commands
static const network_task_config_t network_config = {
    .ssid = WIFI_SSID,
    .password = WIFI_PASSWORD,
    .client_id = "SE4_MASTER_NODE", 
    .subscription = "se4/memorygame/+/command", // Listen to all slave nodes
    .status_topic = MQTT_PRESENCE_TOPIC,
};

static void application_task(void *arg) {
    (void)arg;
    led_init();

    // 1. Initialize Master Game Logic
    game_init(&master_game);
    printf("Master Game Manager initialized and board shuffled.\n");

    for (;;) {
        // default purple visual
        for (int i = 0; i < NUM_LEDS; ++i) {
            put_pixel(urgb_u32(5, 0, 5));
        }

        // --- 1. LOCAL IMU TAP (If someone physically hits the Master Node) ---
        CompactTapData tap;
        if (xQueueReceive(tapEventQueue, &tap, 0) == pdPASS && tap.tap_event && !tap.imu_fault) {
            if (xSemaphoreTake(game_state_mutex, portMAX_DELAY) == pdTRUE) {
                printf("Master physically tapped (Node %d)\n", NODE_ID);
                game_process_input(&master_game, NODE_ID);
                game_evaluate_match(&master_game);
                
                // Broadcast updated game state to all slaves
                network_publish("se4/game/state", &master_game, sizeof(GameManager), 1, true);
                xSemaphoreGive(game_state_mutex);
            }
        }

        // --- 2. NETWORK TAPS (From Slave Nodes) ---
        mqtt_message_t incoming;
        while (network_receive(&incoming, 0)) {
            // Check if the payload is a tap event
            if (strstr(incoming.payload, "TAP DETECTED") != NULL) {
                
                // Extract which slave panel sent the tap from the topic string
                int panel_id = -1;
                sscanf(incoming.topic, "se4/memorygame/node%d", &panel_id);

                if (panel_id >= 0 && panel_id <= 5) {
                    if (xSemaphoreTake(game_state_mutex, portMAX_DELAY) == pdTRUE) {
                        printf("Master received network tap from Panel: %d\n", panel_id);
                        
                        // Process the rules
                        game_process_input(&master_game, (uint8_t)panel_id);
                        game_evaluate_match(&master_game);

                        // Broadcast the Minimum-Bit state payload back to the network
                        network_publish("se4/game/state", &master_game, sizeof(GameManager), 1, true);
                        xSemaphoreGive(game_state_mutex);
                    }
                }
            } else {
                printf("Received unhandled MQTT message on %s\n", incoming.topic);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(APPLICATION_INTERVAL_MS));
    }
}

int main(void) {
    stdio_init_all();
    
    // Give serial monitor a moment to connect
    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    printf("\n--- Pico W Boot (MASTER NODE) ---\n");

    // Create Game Logic Mutex
    game_state_mutex = xSemaphoreCreateMutex();
    if (game_state_mutex == NULL) {
        printf("Could not create Mutex.\n");
        return 1;
    }

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