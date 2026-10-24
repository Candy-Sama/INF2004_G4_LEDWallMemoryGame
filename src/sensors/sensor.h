// sensor.h
#pragma once
#include <stdbool.h>
#include "FreeRTOS.h"
#include "queue.h"
typedef enum { APP_EVT_MOTION } app_event_t;
bool sensor_start(void);
QueueHandle_t sensor_get_event_queue(void);   // net.c reads motion events from here