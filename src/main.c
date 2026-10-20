/* NOTE FROM AFI: make sure to downaloed the FreeRTOS Kernel. Use this command in the terminal.
mkdir lib
cd lib
git clone --recurse-submodules https://github.com/FreeRTOS/FreeRTOS-Kernel.git
cd .. 
*/

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/cyw43_arch.h"
#include "ws2812.pio.h" 
#include "mqtt_handler.h"
#include "wifi_manager.h"

// --- ADDED FREERTOS & IMU INCLUDES ---
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "imudriver.h"

#define LED_PIN 2       
#define NUM_LEDS 64     
// Removed I2C pin definitions from here, as imu_init() handles them now

// --- ADDED GLOBAL QUEUE ---
QueueHandle_t tapEventQueue;

static inline void put_pixel(uint32_t pixel_grb) {
    pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
}

static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)(r) << 8) | ((uint32_t)(g) << 16) | (uint32_t)(b);
}

/* ==========================================================================
   NETWORK & DISPLAY TASK
   ========================================================================== */
void NetworkDisplayTask(void *pvParameters) {
    // PIO WS2812 Initialization
    PIO pio = pio0;
    int sm = 0;
    uint offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, sm, offset, LED_PIN, 800000, false);
    printf("Loaded WS2812 PIO program at %d on GP%d\n", offset, LED_PIN);

    // Network Setup
    if (wifi_init_and_connect()) {
        cyw43_arch_lwip_begin();
        init_mqtt_client("SE4_Pico_Node_1");
        cyw43_arch_lwip_end();

        printf("Waiting for initial MQTT handshake...\n");
        while (!is_mqtt_connected()) {
            vTaskDelay(pdMS_TO_TICKS(250)); // Replaced sleep_ms with FreeRTOS delay
            cyw43_arch_poll(); 
        }
    }

    // The main loop for this specific task
    while (true) {
        if (!is_wifi_connected()) {
            printf("Wi-Fi connection lost! Waiting for hotspot...\n");
            vTaskDelay(pdMS_TO_TICKS(2000)); 
        } 
        else if (!is_mqtt_connected()) {
            printf("MQTT disconnected! Reconnecting to HiveMQ...\n");
            cyw43_arch_lwip_begin();
            init_mqtt_client("SE4_Pico_Node_1");
            cyw43_arch_lwip_end();
            vTaskDelay(pdMS_TO_TICKS(2000)); 
        } 
        else {
            // Render LED frame
            for (int i = 0; i < NUM_LEDS; ++i) {
                put_pixel(urgb_u32(5, 0, 5)); 
            }
            
            // --- NEW: Check the FreeRTOS Queue for taps from your IMU Task ---
            CompactTapData receivedTap;
            // Only publish if a tap was actually detected (0 block time means it doesn't wait)
            if (xQueueReceive(tapEventQueue, &receivedTap, 0) == pdPASS) {
                if (receivedTap.tap_event == 1) {
                    cyw43_arch_lwip_begin();
                    // In the future, Mustaqim will update this to send the compact binary payload
                    publish_message("se4/memorygame/node1", "TAP DETECTED");
                    cyw43_arch_lwip_end();
                }
            }
        }

        cyw43_arch_poll(); 
        vTaskDelay(pdMS_TO_TICKS(100)); // Free the CPU for the IMU task
    }
}

/* ==========================================================================
   MAIN ENTRY POINT
   ========================================================================== */
int main() {
    stdio_init_all();
    
    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    sleep_ms(1000); 
    
    printf("\n\n--- PICO W BOOT SEQUENCE ---\n");

    // 1. Create the FreeRTOS Queue for your compact bit-field struct
    tapEventQueue = xQueueCreate(10, sizeof(CompactTapData));

    // 2. Initialize your IMU module (This also creates the IMUSensorTask inside imudriver.c)
    // Note: I removed the redundant raw i2c_init code from main() since imu_init handles it
    imu_init(1); // Passing '1' as the Node ID

    // 3. Create the Network & Display task
    xTaskCreate(NetworkDisplayTask, "NetDisplayTask", 1024, NULL, 2, NULL);

    // 4. Start the FreeRTOS Scheduler (Main thread ends here and tasks take over)
    printf("Starting FreeRTOS Scheduler...\n");
    vTaskStartScheduler();

    // The code should never reach this point
    while(1);
}
