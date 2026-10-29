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


#define I2C_PORT i2c0
#define I2C_SDA 4
#define I2C_SCL 1
#define SENSOR_TASK_PRIORITY 3
#define SENSOR_TASK_STACK    1024

static QueueHandle_t sensor_event_q;
static volatile orientation_t s_orientation = ORIENT_UNKNOWN;
static volatile int s_pending_turns = -1;

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
    if (!setup_sensor()) {
        printf("IMU not available; sensor task stopping.\n");
        vTaskDelete(NULL);
    }

    motion_detector_t detector;
    memset(&detector, 0, sizeof(detector));
    orientation_detector_t orient;
    orientation_detector_init(&orient);
#if ENABLE_ORIENTATION_SYNC
    uint8_t last_shared_turns = 0;
#endif
#if IMU_LOG_READINGS
    TickType_t next_print = xTaskGetTickCount();
#endif

    for (;;) {
        imu_acceleration_t sample;
        imu_read_result_t result = imu_read_acceleration(&sample);

        if (result == IMU_READ_ERROR) {
            detector.initialised = false;
            orientation_detector_init(&orient);   // do not trust an old pose after a read error
            s_orientation = ORIENT_UNKNOWN;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (result == IMU_NEW_DATA) {
#if IMU_LOG_READINGS
            if ((int32_t)(xTaskGetTickCount() - next_print) >= 0) {
                printf("Node %d IMU: X=%d mg Y=%d mg Z=%d mg\n",
                       identity_node_id(), sample.x_mg, sample.y_mg, sample.z_mg);
                next_print = xTaskGetTickCount() + pdMS_TO_TICKS(500);
            }
#endif
            uint32_t now_ms = to_ms_since_boot(get_absolute_time());
 
            // ---- orientation: confirm a stable pose, rotate images, share it ----
            if (orientation_update(&orient, sample.x_mg, sample.y_mg, sample.z_mg, now_ms)) {
#if ENABLE_ORIENTATION_SYNC
                bool first_fix = (s_orientation == ORIENT_UNKNOWN);
#endif
                s_orientation = orient.stable;
                printf("Node %d orientation: %s\n", identity_node_id(),
                       orientation_name(orient.stable));
#if ENABLE_AUTO_ROTATE
                display_set_rotation(orient.image_turns);
#endif
#if ENABLE_ORIENTATION_SYNC
                // Share only a real, user-caused change. The first fix after boot is
                // not shared, otherwise a board powering up upright would overwrite
                // the wall's retained rotation. Flat poses keep the same rotation
                // (image_turns unchanged), so they are not shared either.
                if (!first_fix && orient.image_turns != last_shared_turns) {
                    taskENTER_CRITICAL();
                    s_pending_turns = orient.image_turns;
                    taskEXIT_CRITICAL();
                }
                last_shared_turns = orient.image_turns;
#endif
            }
            bool moved = motion_detector_update(&detector, sample.x_mg, sample.y_mg,
                                                sample.z_mg,
                                                to_ms_since_boot(get_absolute_time()));
            if (moved && ENABLE_MOTION_TEST) {
                app_event_t evt = APP_EVT_MOTION;
                xQueueSend(sensor_event_q, &evt, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

QueueHandle_t sensor_get_event_queue(void) { return sensor_event_q; }
orientation_t sensor_get_orientation(void) { return s_orientation; }

bool sensor_take_orientation_change(uint8_t *image_turns) {
    if (image_turns == NULL) return false;
    int value;
    taskENTER_CRITICAL();
    value = s_pending_turns;
    s_pending_turns = -1;
    taskEXIT_CRITICAL();
    if (value < 0) return false;
    *image_turns = (uint8_t)value;
    return true;
}


bool sensor_start(void) {
    sensor_event_q = xQueueCreate(8, sizeof(app_event_t));
    if (!sensor_event_q) return false;
    return xTaskCreate(sensor_task, "sensor", SENSOR_TASK_STACK, NULL,
                       SENSOR_TASK_PRIORITY, NULL) == pdPASS;
}