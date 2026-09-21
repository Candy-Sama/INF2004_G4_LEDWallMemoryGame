#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/cyw43_arch.h"
#include "ws2812.pio.h" 
#include "mqtt_handler.h"
#include "wifi_manager.h" // Added your new Wi-Fi module

#define LED_PIN 2       
#define NUM_LEDS 64     
#define I2C_PORT i2c0
#define I2C_SDA 8
#define I2C_SCL 9

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

    // PIO WS2812 Initialization
    PIO pio = pio0;
    int sm = 0;
    uint offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, sm, offset, LED_PIN, 800000, false);
    printf("Loaded WS2812 PIO program at %d on GP%d\n", offset, LED_PIN);

    // Network Setup using the new subsystem
    if (wifi_init_and_connect()) {
        cyw43_arch_lwip_begin();
        init_mqtt_client("SE4_Pico_Node_1");
        cyw43_arch_lwip_end();
    }

    while (true) {
        for (int i = 0; i < NUM_LEDS; ++i) {
            put_pixel(urgb_u32(8, 0, 0)); 
        }
        
        // Publish test message
        cyw43_arch_lwip_begin();
        publish_message("se4/memorygame/node1", "Node 1 is active!");
        cyw43_arch_lwip_end();

        cyw43_arch_poll(); 
        sleep_ms(1000);
    }
}