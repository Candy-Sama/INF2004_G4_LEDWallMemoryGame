// sensor.h
#pragma once
#include <stdbool.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "imu_orientation.h"

typedef enum {
    APP_EVT_MOTION_SINGLE,
    APP_EVT_MOTION_DOUBLE,
    APP_EVT_MOTION_TRIPLE
} app_event_t;
bool sensor_start(void);
QueueHandle_t sensor_get_event_queue(void);   // net.c reads motion events from here
orientation_t sensor_get_orientation(void);