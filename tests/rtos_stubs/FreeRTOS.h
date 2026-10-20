#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define configASSERT(x) assert(x)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#endif
