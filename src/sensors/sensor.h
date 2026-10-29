// sensor.h
#pragma once
#include <stdbool.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "imu_orientation.h"

typedef enum { APP_EVT_MOTION } app_event_t;
bool sensor_start(void);
QueueHandle_t sensor_get_event_queue(void);   // net.c reads motion events from here
orientation_detector_t* sensor_get_orientation_detector(void);
bool sensor_take_orientation_change(uint8_t *image_turns);