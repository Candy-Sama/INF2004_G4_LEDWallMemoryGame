#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/i2c.h"
#include "node_config.h"
#include "wifi_manager.h"
#include "mqtt_handler.h"
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

// Runs in the main loop, not in a network callback.
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
    if (!is_wifi_connected() || !is_mqtt_connected()) {
        printf("Movement ignored: network is disconnected.\n");
        return;
    }
    const char *colour = send_green_next ? "GREEN" : "RED";
    if (mqtt_send(MQTT_PEER_COMMAND_TOPIC, colour, strlen(colour))) {
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
    for (size_t i = 0; i < MQTT_RX_QUEUE_CAPACITY && mqtt_receive(&message); ++i) {
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
    uint32_t drops = mqtt_received_drop_count();
    if (drops != previous_drops) {
        printf("MQTT: %lu received messages discarded in total.\n", (unsigned long)drops);
        previous_drops = drops;
    }
}

static void publish_status(void) {
    static absolute_time_t next_publish;
    if (time_reached(next_publish)) {
        mqtt_send(MQTT_STATUS_TOPIC, MQTT_STATUS_MESSAGE, strlen(MQTT_STATUS_MESSAGE));
        next_publish = make_timeout_time_ms(1000);
    }
}

int main(void) {
    stdio_init_all();
    // Open the USB serial monitor to start this teaching/demo firmware.
    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    sleep_ms(1000);

    printf("\n--- NODE %d: SENSOR / MQTT / LED DEMO ---\n", NODE_ID);
    bool sensor_available = setup_sensor();
    ledmatrix_init();

    // Client IDs identify physical boards; NODE_ID chooses their MQTT topics.
    char board_id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    static char client_id[32];
    pico_get_unique_board_id_string(board_id, sizeof(board_id));
    snprintf(client_id, sizeof(client_id), "SE4_%s", board_id);
    printf("MQTT client: %s\nListen: %s\n", client_id, MQTT_COMMAND_TOPIC);

    if (!wifi_init_and_connect()) {
        printf("Wi-Fi setup failed. Check credentials/network and restart.\n");
        return 1;
    }
    // The application supplies the topic. The identity/layout module can supply
    // a different string here without changing mqtt_handler.c.
    if (!mqtt_subscribe_topic(MQTT_COMMAND_TOPIC)) {
        printf("Could not register the command topic.\n");
        return 1;
    }
    // Preserve the dim pink startup colour used by the working demo.
    bool display_started = false;
    bool wifi_was_connected = true;

    while (true) {
        // 1. Read the sensor. Only node 1 publishes movement commands.
        if (sensor_available && read_movement() && ENABLE_MOTION_TEST && NODE_ID == 1) {
            send_movement();
        }

        // 2. Maintain MQTT when Wi-Fi is available.
        bool wifi_connected = is_wifi_connected();
        if (wifi_connected != wifi_was_connected) {
            printf("Wi-Fi %s.\n", wifi_connected ? "available" : "lost; restore network or restart");
            wifi_was_connected = wifi_connected;
        }
        if (wifi_connected) {
            // Keep servicing even after connecting: subscriptions may need retries.
            init_mqtt_client(client_id);
            if (mqtt_is_ready()) {
                if (!display_started) {
                    ledmatrix_fill(5, 0, 5);
                    display_started = true;
                }
                // 3. Apply incoming commands and send the status message.
                apply_received_command();
                publish_status();
            }
        }

        // The configured Pico SDK architecture services networking in the background.
        sleep_ms(10);
    }
}
