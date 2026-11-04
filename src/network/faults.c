#include "faults.h"
#include "FreeRTOS.h"
#include "task.h"

static volatile uint8_t s_faults;

void fault_set(uint8_t mask) {
    taskENTER_CRITICAL();
    s_faults |= (uint8_t)(mask & FAULT_MASK);
    taskEXIT_CRITICAL();
}

void fault_clear(uint8_t mask) {
    taskENTER_CRITICAL();
    s_faults &= (uint8_t)~(mask & FAULT_MASK);
    taskEXIT_CRITICAL();
}

uint8_t fault_get(void) { return s_faults; }
