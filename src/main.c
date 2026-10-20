#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/i2c.h"
#include "node_config.h"
#include "wifi_creds.h" // Standard build: application supplies configured credentials.
#include "network_task.h"
#include "task.h"
#include "pico/rand.h"
#include "network_recovery.h"
#include "imudriver.h"
#include "motion_detector.h"
#include "ledmatrix.h"

#define I2C_PORT i2c0
#define I2C_SDA 0
#define I2C_SCL 1

static bool setup_sensor(void) {
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    printf("IMU: SDA=GP%d, SCL=GP%d\n", I2C_SDA, I2C_SCL);
    return imu_init(I2C_PORT);
}

// Runs in the Application Task, not in a network callback.
static bool read_movement(void) {
    static absolute_time_t next_read;
    static motion_detector_t detector;
#if IMU_LOG_READINGS
    static absolute_time_t next_print;
#endif
    if (!time_reached(next_read)) {
        return false;
    }
    next_read = make_timeout_time_ms(10);

    imu_acceleration_t sample;
    imu_read_result_t result = imu_read_acceleration(&sample);
    if (result == IMU_READ_ERROR) {
        detector.initialised = false;
        next_read = make_timeout_time_ms(500);
        return false;
    }
    if (result != IMU_NEW_DATA) {
        return false;
    }
#if IMU_LOG_READINGS
    if (time_reached(next_print)) {
        printf("Node %d IMU: X=%d mg Y=%d mg Z=%d mg\n",
               NODE_ID, sample.x_mg, sample.y_mg, sample.z_mg);
        next_print = make_timeout_time_ms(500);
    }
#endif
    return motion_detector_update(&detector, sample.x_mg, sample.y_mg,
                                  sample.z_mg, to_ms_since_boot(get_absolute_time()));
}

static void send_movement(void) {
    static bool send_green_next = true;
    if (!network_game_ready()) {
        printf("Movement ignored: network is disconnected.\n");
        return;
    }
    const char *colour = send_green_next ? "GREEN" : "RED";
    if (network_publish(MQTT_PEER_COMMAND_TOPIC, colour, strlen(colour), 0, false)) {
        printf("Movement: queued %s to %s\n", colour, MQTT_PEER_COMMAND_TOPIC);
        send_green_next = !send_green_next;
    } else {
        printf("Movement: publish failed; try again.\n");
    }
}

// Application-specific interpretation stays out of the MQTT transport.
static void apply_received_command(void) {
    mqtt_message_t message;
    // Bound each batch so incoming traffic cannot starve sensor sampling.
    for (size_t i = 0; i < MQTT_RX_QUEUE_CAPACITY && network_receive(&message, 0); ++i) {
        if (!strcmp(message.topic, MQTT_STATE_TOPIC)) {
            // Example snapshot only: one byte holds a 2-bit colour code.
            // Real game/layout owners replace this with full state installation.
            if (message.length != RECOVERY_HEADER_SIZE + 1) continue;
            uint8_t colour = message.payload[RECOVERY_HEADER_SIZE];
            if (colour > 3) continue;
            ledmatrix_fill(colour == 1 || colour == 3 ? 5 : 0,
                           colour == 2 ? 5 : 0, colour == 3 ? 5 : 0);
            if (!network_state_applied(&message))
                printf("State applied but acknowledgement queue full; recovery will retry.\n");
            continue;
        }
        if (strcmp(message.topic, MQTT_COMMAND_TOPIC) != 0) continue;
        if (message.length == 3 && memcmp(message.payload, "RED", 3) == 0) {
            ledmatrix_fill(5, 0, 0);
            printf("LED command applied: RED\n");
        } else if (message.length == 5 && memcmp(message.payload, "GREEN", 5) == 0) {
            ledmatrix_fill(0, 5, 0);
            printf("LED command applied: GREEN\n");
        } else if (message.length == 3 && memcmp(message.payload, "OFF", 3) == 0) {
            ledmatrix_fill(0, 0, 0);
            printf("LED command applied: OFF\n");
        } else {
            printf("Demo: unrecognised payload on %s (%u bytes).\n",
                   message.topic, (unsigned)message.length);
        }
    }
    static uint32_t previous_drops;
    uint32_t drops = network_drop_count();
    if (drops != previous_drops) {
        printf("Network: %lu queued messages discarded in total.\n", (unsigned long)drops);
        previous_drops = drops;
    }
}

// Demo hardware/parser lives here; networking only transports its bytes.
static void application_task(void *arg) {
    (void)arg;
    bool sensor_available = setup_sensor();
    ledmatrix_init();
    bool display_started = false;
    for (;;) {
        if (sensor_available && read_movement() && ENABLE_MOTION_TEST && NODE_ID == 1)
            send_movement();
        if (network_ready()) {
            if (!display_started) {
                ledmatrix_fill(5, 0, 5);
                display_started = true;
            }
            apply_received_command();
        } else {
            if (display_started) ledmatrix_fill(0, 0, 0);
            display_started = false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    panic("FreeRTOS stack overflow: %s", name);
}
void vApplicationMallocFailedHook(void) { panic("FreeRTOS heap exhausted"); }
int main(void) {
    stdio_init_all();
    // Boot without a serial monitor so unattended panels can reconnect.
    char board_id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    static char client_id[32];
    static network_task_config_t network;
    pico_get_unique_board_id_string(board_id, sizeof(board_id));
    snprintf(client_id, sizeof(client_id), "SE4_%s", board_id);
    network = (network_task_config_t){
        .ssid = WIFI_SSID, .password = WIFI_PASSWORD, .client_id = client_id,
        .subscription = MQTT_COMMAND_TOPIC, .status_topic = MQTT_PRESENCE_TOPIC,
        .sync_topic = MQTT_SYNC_TOPIC, .state_topic = MQTT_STATE_TOPIC,
        .applied_topic = MQTT_APPLIED_TOPIC, .resume_topic = MQTT_RESUME_TOPIC,
        .boot_nonce = get_rand_64() | 1u
    };
    if (!network_task_start(&network) ||
        xTaskCreate(application_task, "Application", 1024, NULL, 2, NULL) != pdPASS)
        panic("Could not create application tasks");
    // Start scheduling. Successful startup never returns to main().
    vTaskStartScheduler();
    panic("FreeRTOS scheduler returned");
}
