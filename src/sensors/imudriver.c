#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

// FreeRTOS Includes
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

// Your custom header where CompactTapData is defined
#include "imudriver.h"

// Hardware Definitions (Adjust IMU_INT_PIN to match your wiring)
#define I2C_PORT i2c0
#define I2C_SDA_PIN 0
#define I2C_SCL_PIN 1
#define IMU_INT_PIN 2          // Example: The Pico pin connected to the IMU's INT output
#define DEBOUNCE_DELAY_MS 400  // Ignore physical vibrations for 400ms after a tap

// Global handles
TaskHandle_t sensorTaskHandle = NULL;
extern QueueHandle_t tapEventQueue; // The queue created in main.c

uint8_t currentNodeId = 0; // Will be set during initialization

/* ==========================================================================
   1. HARDWARE INTERRUPT SERVICE ROUTINE (ISR)
   This replaces the ±0.2s blocking loop. It fires instantly when the IMU 
   hardware detects a physical shock and pulses the INT pin.
   ========================================================================== */
void imu_gpio_isr(uint gpio, uint32_t events) {
    if (gpio == IMU_INT_PIN) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        
        // Instantly wake up the sleeping FreeRTOS Sensor Task
        vTaskNotifyGiveFromISR(sensorTaskHandle, &xHigherPriorityTaskWoken);
        
        // Yield if the Sensor Task has a higher priority than the currently running task
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/* ==========================================================================
   2. FREERTOS SENSOR TASK
   Sleeps at 0% CPU usage until woken by the ISR. Debounces the input, 
   packs the bits, and safely queues the data for the Network Task.
   ========================================================================== */
void IMUSensorTask(void *pvParameters) {
    CompactTapData tapData;
    TickType_t lastTapTime = 0;

    for (;;) {
        // Sleep indefinitely until the hardware ISR sends a notification
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        TickType_t currentTime = xTaskGetTickCount();

        // Software Debounce: Ignore if it hasn't been long enough since the last tap
        // This solves the "vibrating shared wall" and "double-tap" risks
        if ((currentTime - lastTapTime) > pdMS_TO_TICKS(DEBOUNCE_DELAY_MS)) {
            lastTapTime = currentTime;

            // Pack the Minimum-Bit Data Representation (Change Req 1)
            tapData.node_id = currentNodeId;
            tapData.tap_event = 1;
            tapData.imu_fault = 0; // Set to 1 if you add I2C error checking later
            tapData.reserved = 0;

            // Send BY VALUE to the FreeRTOS Queue to prevent Race Conditions
            if (tapEventQueue != NULL) {
                xQueueSend(tapEventQueue, &tapData, 0);
            }
            
            printf("Valid tap detected on Node %d! Data packed and queued.\n", currentNodeId);
        } else {
            // It was a false trigger (vibration or double tap) - ignore it.
            printf("Ignored false tap/vibration.\n");
        }
    }
}

/* ==========================================================================
   3. INITIALIZATION FUNCTION
   Called once from main() to set up I2C, pins, and launch the task.
   ========================================================================== */
void imu_init(uint8_t node_id) {
    currentNodeId = node_id;

    // Initialize I2C0 at 400 kHz
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    // Initialize IMU Interrupt Pin
    gpio_init(IMU_INT_PIN);
    gpio_set_dir(IMU_INT_PIN, GPIO_IN);
    
    // Create the FreeRTOS Task (Stack size 256, Priority 3)
    xTaskCreate(IMUSensorTask, "IMUSensorTask", 256, NULL, 3, &sensorTaskHandle);

    // Attach the hardware ISR to the INT pin (Falling or Rising edge depends on your IMU model)
    gpio_set_irq_enabled_with_callback(IMU_INT_PIN, GPIO_IRQ_EDGE_FALL, true, &imu_gpio_isr);
    
    printf("IMU Driver Initialized for Node %d\n", currentNodeId);
}
