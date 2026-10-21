// FreeRTOS smoke test. Build with: cmake -DRTOS_SMOKETEST=ON ..
// Proves: kernel runs, tasks switch, queues work, cyw43 + lwIP-in-FreeRTOS init OK.
// Expected: Pico W onboard LED blinks; USB serial prints "tick N" lines.
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

static QueueHandle_t blink_q;

static void producer_task(void *arg) {
    (void)arg;
    uint32_t n = 0;
    for (;;) {
        xQueueSend(blink_q, &n, 0);
        n++;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void blink_task(void *arg) {
    (void)arg;
    // Must happen inside a task when using pico_cyw43_arch_lwip_sys_freertos.
    if (cyw43_arch_init()) {
        printf("cyw43_arch_init FAILED\n");
        vTaskDelete(NULL);
    }
    printf("cyw43 + lwIP (FreeRTOS) initialised\n");
    uint32_t n;
    bool on = false;
    for (;;) {
        if (xQueueReceive(blink_q, &n, portMAX_DELAY) == pdTRUE) {
            on = !on;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
            printf("tick %lu\n", (unsigned long)n);
        }
    }
}

void vApplicationMallocFailedHook(void) { for (;;) {} }
void vApplicationStackOverflowHook(TaskHandle_t t, char *name) { (void)t; (void)name; for (;;) {} }

int main(void) {
    stdio_init_all();
    blink_q = xQueueCreate(4, sizeof(uint32_t));
    xTaskCreate(producer_task, "producer", 512,  NULL, 2, NULL);
    xTaskCreate(blink_task,    "blink",    1024, NULL, 2, NULL);
    vTaskStartScheduler();
    for (;;) {}
}