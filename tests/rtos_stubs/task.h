#include "FreeRTOS.h"
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, TaskHandle_t *handle);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
