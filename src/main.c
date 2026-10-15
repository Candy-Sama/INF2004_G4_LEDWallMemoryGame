#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/cyw43_arch.h"
#include "ws2812.pio.h" 
#include "mqtt_handler.h"
#include "imudriver.h"
#include "wifi_manager.h" // Added your new Wi-Fi module

#define LED_PIN 2       
#define NUM_LEDS 64     
#define I2C_PORT i2c0
#define I2C_SDA 0
#define I2C_SCL 1

static inline void put_pixel(uint32_t pixel_grb) {
    pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
}

static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)(r) << 8) | ((uint32_t)(g) << 16) | (uint32_t)(b);
}

int main() {
    stdio_init_all();
    
    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    sleep_ms(1000); // Give the UI 1 second to catch up
    
    printf("\n\n--- PICO W BOOT SEQUENCE ---\n");

    // I2C Initialisation for IMU
    i2c_init(I2C_PORT, 400*1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    printf("IMU: checking SDA=GP%d, SCL=GP%d...\n", I2C_SDA, I2C_SCL);
    bool imu_available = imu_init(I2C_PORT);

    // PIO WS2812 Initialization
    PIO pio = pio0;
    int sm = 0;
    uint offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, sm, offset, LED_PIN, 800000, false);
    printf("Loaded WS2812 PIO program at %d on GP%d\n", offset, LED_PIN);

    // Stable per-board ID prevents two Picos disconnecting each other.
    char board_id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    static char client_id[32];
    pico_get_unique_board_id_string(board_id, sizeof(board_id));
    snprintf(client_id, sizeof(client_id), "SE4_%s", board_id);
    printf("MQTT client ID: %s\n", client_id);
    printf("Node %d: status=%s, commands=%s\n", NODE_ID,
           MQTT_STATUS_TOPIC, MQTT_COMMAND_TOPIC);

    if (!wifi_init_and_connect()) {
        printf("Wi-Fi setup failed. Check credentials/network and restart the Pico.\n");
        return 1;
    }

    uint32_t led_colour = urgb_u32(5, 0, 5);
    bool display_dirty = true;
    absolute_time_t next_publish = get_absolute_time();
    absolute_time_t next_imu_print = get_absolute_time();
#if ENABLE_PEER_TEST && NODE_ID == 1
    bool peer_test_started = false;
    bool peer_test_sent = false;
    absolute_time_t peer_test_due;
#endif

   while (true) {
        // Diagnostic readings stay separate from MQTT and LED commands.
        if (imu_available && time_reached(next_imu_print)) {
            imu_acceleration_t acceleration;
            imu_read_result_t result = imu_read_acceleration(&acceleration);
            if (result == IMU_NEW_DATA) {
                printf("Node %d IMU: X=%d mg Y=%d mg Z=%d mg\n", NODE_ID,
                       acceleration.x_mg, acceleration.y_mg, acceleration.z_mg);
            } else if (result == IMU_READ_ERROR) {
                printf("IMU: read failed; check connections. Will retry.\n");
            } else {
                printf("IMU: waiting for fresh acceleration data.\n");
            }
            next_imu_print = make_timeout_time_ms(500);
        }
        // 1. Check Wi-Fi Health
        if (!is_wifi_connected()) {
            printf("Wi-Fi connection lost! Waiting for hotspot...\n");
            // Optional: call wifi_init_and_connect() here if it supports auto-retries
            sleep_ms(2000); 
        } 
        // 2. Check MQTT Health
        else if (!is_mqtt_connected()) {
            cyw43_arch_lwip_begin();
            init_mqtt_client(client_id);
            cyw43_arch_lwip_end();
        } 
        // 3. Network is healthy. Safe to render and publish!
        else {
#if ENABLE_PEER_TEST && NODE_ID == 1
            // Start node 2 first: this non-retained command is not saved for later.
            if (!peer_test_started) {
                peer_test_due = make_timeout_time_ms(3000);
                peer_test_started = true;
            }
            if (!peer_test_sent && time_reached(peer_test_due)) {
                cyw43_arch_lwip_begin();
                peer_test_sent = publish_message(MQTT_PEER_COMMAND_TOPIC, "GREEN");
                cyw43_arch_lwip_end();
                if (peer_test_sent) {
                    printf("Peer test: queued GREEN to %s (once per boot).\n",
                           MQTT_PEER_COMMAND_TOPIC);
                } else {
                    peer_test_due = make_timeout_time_ms(1000);
                }
            }
#endif
            // Network callbacks only store commands. Hardware updates happen here.
            led_command_t command = mqtt_take_led_command();
            if (command == LED_COMMAND_RED) {
                led_colour = urgb_u32(5, 0, 0);
                printf("LED command applied: RED\n");
            } else if (command == LED_COMMAND_GREEN) {
                led_colour = urgb_u32(0, 5, 0);
                printf("LED command applied: GREEN\n");
            } else if (command == LED_COMMAND_OFF) {
                led_colour = 0;
                printf("LED command applied: OFF\n");
            }
            display_dirty |= command != LED_COMMAND_NONE;
            if (display_dirty) {
                for (int i = 0; i < NUM_LEDS; ++i) {
                    put_pixel(led_colour);
                }
                display_dirty = false;
            }
            
            // Publish test message
            if (time_reached(next_publish)) {
                cyw43_arch_lwip_begin();
                publish_message(MQTT_STATUS_TOPIC, MQTT_STATUS_MESSAGE);
                cyw43_arch_lwip_end();
                next_publish = make_timeout_time_ms(1000);
            }
        }

        // Required background hardware polling
        cyw43_arch_poll(); 
        sleep_ms(10); // React to commands promptly; heartbeat still runs once a second.
    }
}
