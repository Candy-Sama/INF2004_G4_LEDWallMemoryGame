#include "sensor.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "task.h"
#include "node_config.h"
#include "node_identity.h"
#include "imudriver.h"
#include "motion_detector.h"
#include "imu_orientation.h"
#include "display.h"
#include "fatal.h"


#define I2C_PORT i2c0
#define I2C_SDA 4
#define I2C_SCL 1
#define SENSOR_TASK_PRIORITY 3
#define SENSOR_TASK_STACK    1024
#define IMU_INIT_ATTEMPTS    5
#define IMU_INIT_RETRY_MS    500

static QueueHandle_t sensor_event_q;
static volatile orientation_t s_orientation = ORIENT_UNKNOWN;
// static volatile int s_pending_turns = -1;

static bool setup_sensor(void) {
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    printf("IMU: SDA=GP%d, SCL=GP%d\n", I2C_SDA, I2C_SCL);
    return imu_init(I2C_PORT);
}

static void sensor_task(void *arg) {
    (void)arg;
    bool imu_ok = false;
    for (int attempt = 1; attempt <= IMU_INIT_ATTEMPTS && !imu_ok; attempt++) {
        imu_ok = setup_sensor();
        if (!imu_ok) {
            printf("IMU init failed (attempt %d/%d)\n", attempt, IMU_INIT_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(IMU_INIT_RETRY_MS));
        }
    }
    if (!imu_ok) fatal_reboot("IMU not responding (check SDA/SCL, power, GND)");

    motion_detector_t detector;
    memset(&detector, 0, sizeof(detector));
    tap_classifier_t classifier = {0};
    orientation_detector_t orient;
    orientation_detector_init(&orient);
// #if ENABLE_ORIENTATION_SYNC
//     uint8_t last_shared_turns = 0;
// #endif
#if IMU_LOG_READINGS
    TickType_t next_print = xTaskGetTickCount();
#endif

    for (;;) {
        imu_acceleration_t sample;
        imu_read_result_t result = imu_read_acceleration(&sample);

        if (result == IMU_READ_ERROR) {
            classifier.pending = false;
            detector.initialised = false;
            orientation_detector_init(&orient);   // do not trust an old pose after a read error
            s_orientation = ORIENT_UNKNOWN;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (result == IMU_NEW_DATA) {
#if IMU_LOG_READINGS
            if ((int32_t)(xTaskGetTickCount() - next_print) >= 0) {
                printf("[%lu ms] Node %d IMU: X=%d mg Y=%d mg Z=%d mg\n",
                    (unsigned long)to_ms_since_boot(get_absolute_time()),
                    identity_node_id(),
                    sample.x_mg, sample.y_mg, sample.z_mg);
                next_print = xTaskGetTickCount() + pdMS_TO_TICKS(10);
            }
#endif
            uint32_t now_ms = to_ms_since_boot(get_absolute_time());
                // ---- orientation: this panel only ----
                if (orientation_update(&orient, sample.x_mg, sample.y_mg, sample.z_mg, now_ms)) {
                    s_orientation = orient.stable;
                    printf("Node %d orientation: %s -> image turns %u\n", identity_node_id(),
                            orientation_name(orient.stable), (unsigned)orient.image_turns);
                    display_set_rotation(orient.image_turns);
                }
            bool moved = motion_detector_update(&detector, sample.x_mg, sample.y_mg,
                                                sample.z_mg,
                                                to_ms_since_boot(get_absolute_time()));
            if (moved) {
                printf("[%lu ms] CANDIDATE\n", (unsigned long)now_ms);
            }
            tap_result_t tap = tap_classifier_update(&classifier, moved, now_ms);

                        if (tap == TAP_SINGLE) {
                printf("SINGLE\n");
                app_event_t evt = APP_EVT_MOTION_SINGLE;
                xQueueSend(sensor_event_q, &evt, 0);
            } else if (tap == TAP_DOUBLE) {
                printf("DOUBLE sent\n");
                app_event_t evt = APP_EVT_MOTION_DOUBLE;
                xQueueSend(sensor_event_q, &evt, 0);
            } else if (tap == TAP_TRIPLE) {
                printf("TRIPLE sent\n");
                app_event_t evt = APP_EVT_MOTION_TRIPLE;
                xQueueSend(sensor_event_q, &evt, 0);
            }
        }                                   // end if (result == IMU_NEW_DATA)
        vTaskDelay(pdMS_TO_TICKS(10));
    }                                       // end for (;;)
}                                           // end sensor_task

QueueHandle_t sensor_get_event_queue(void) { return sensor_event_q; }
orientation_t sensor_get_orientation(void) { return s_orientation; }

bool sensor_start(void) {
    sensor_event_q = xQueueCreate(8, sizeof(app_event_t));
    if (!sensor_event_q) return false;
    return xTaskCreate(sensor_task, "sensor", SENSOR_TASK_STACK, NULL,
                       SENSOR_TASK_PRIORITY, NULL) == pdPASS;
}