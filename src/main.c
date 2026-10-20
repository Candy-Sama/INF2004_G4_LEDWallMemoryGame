#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "ws2812.pio.h" 
#include "game_logic.h"

// FreeRTOS Includes
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#define LED_PIN 2       
#define NUM_LEDS 64     
#define I2C_PORT i2c0
#define I2C_SDA 8
#define I2C_SCL 9

// FreeRTOS Handles
QueueHandle_t tap_event_queue;
SemaphoreHandle_t game_state_mutex;

// The central game state using the Minimum-Bit struct
GameManager master_game;

// --- FREE RTOS TASKS ---

void Game_Logic_Task(void *pvParameters) {
    uint8_t received_panel_id;

    while (true) {
        // 1. Wait infinitely for a tap event to arrive in the queue from the network
        if (xQueueReceive(tap_event_queue, &received_panel_id, portMAX_DELAY) == pdPASS) {
            
            // 2. Tap received! Lock the Mutex to protect the shared bit-field data
            if (xSemaphoreTake(game_state_mutex, portMAX_DELAY) == pdTRUE) {
                
                // 3. Process the game rules safely
                printf("Processing tap on panel: %d\n", received_panel_id);
                game_process_input(&master_game, received_panel_id);
                game_evaluate_match(&master_game);
                
                // (Mustaqim's network_publish command will go here later to broadcast the state)
                
                // 4. Unlock the Mutex so the Network task can use it again
                xSemaphoreGive(game_state_mutex);
            }
        }
    }
}

// DUMMY IN PLACE TO TEST MASTER //
void Dummy_Network_Task(void *pvParameters) {
    uint8_t fake_tap;

    // Wait 3 seconds for boot
    vTaskDelay(pdMS_TO_TICKS(3000)); 

    // Simulate Player Tapping Panel 0
    fake_tap = 0;
    printf("[DUMMY NETWORK] Sending Tap for Panel %d to Queue...\n", fake_tap);
    xQueueSend(tap_event_queue, &fake_tap, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second

    // Simulate Player Double-Tapping Panel 0 to cheat
    fake_tap = 0;
    printf("[DUMMY NETWORK] Sending Double-Tap for Panel %d to Queue...\n", fake_tap);
    xQueueSend(tap_event_queue, &fake_tap, portMAX_DELAY);
    
    // The task deletes itself when finished
    vTaskDelete(NULL); 
}

// --- MAIN BOOT SEQUENCE ---

int main() {
    stdio_init_all();
    
    // Wait for Serial Monitor to connect so we don't miss boot messages
    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    sleep_ms(1000); 
    
    printf("\n\n--- PICO W BOOT SEQUENCE (FreeRTOS) ---\n");

    // Initialize I2C Hardware
    i2c_init(I2C_PORT, 400*1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    // Initialize WS2812 PIO Hardware
    PIO pio = pio0;
    int sm = 0;
    uint offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, sm, offset, LED_PIN, 800000, false);
    printf("Loaded WS2812 PIO program at %d on GP%d\n", offset, LED_PIN);

    // 1. Initialize the Master Game Logic
    game_init(&master_game);
    printf("Game Manager initialized and board shuffled.\n");

    // 2. Create the FreeRTOS Mutex
    game_state_mutex = xSemaphoreCreateMutex();
    if (game_state_mutex == NULL) {
        printf("CRITICAL ERROR: Failed to create Mutex!\n");
        while(1); 
    }

    // 3. Create the FreeRTOS Queue (Holds up to 10 tap events)
    tap_event_queue = xQueueCreate(10, sizeof(uint8_t));
    if (tap_event_queue == NULL) {
        printf("CRITICAL ERROR: Failed to create Queue!\n");
        while(1); 
    }

    // 4. Spawn the Game Logic Task at Priority 2
    xTaskCreate(Game_Logic_Task, "GameLogic", 1024, NULL, 2, NULL);
    //FAKE DUMMY TASK. DELETE WHEN DONE
    // Spawn the temporary testing task
    xTaskCreate(Dummy_Network_Task, "DummyNet", 1024, NULL, 3, NULL);

    // 5. Hand control over to the FreeRTOS Operating System
    printf("Starting FreeRTOS Scheduler...\n");
    vTaskStartScheduler();

    // The microcontroller will never reach this loop because FreeRTOS is now in control
    while (true) {}
}