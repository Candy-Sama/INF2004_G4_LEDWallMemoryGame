#include <stdio.h> // Provides printf
#include "pico/stdlib.h" // Gives basic Pico types such as uint
#include "hardware/pio.h" // Gives the PIO types and functions
#include "ws2812.pio.h" // Holds the PIO program
#include "ledmatrix.h" // Provides the LED pin

// Default Brightness
static uint8_t global_brightness = 2;
void set_brightness(uint8_t brightness) {
    global_brightness = brightness;
}

// Turns on the LED panel so it can receives colors
void led_init(void) {
    PIO pio = pio0;
    int sm = 0;
    uint offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, sm, offset, LED_PIN, 800000, false);
    printf("Loaded WS2812 PIO program at %d on GP%d\n", offset, LED_PIN);
}

uint32_t frame[NUM_LEDS];

// Set all pixels to 0
void clear_frame(void){
    for(int i = 0; i < NUM_LEDS; i++){
        frame[i] = 0;
    }
}

// Put one color from memory to the panel 
void set_pixel(int x, int y, uint32_t color){
    if (x < 0 || y < 0 || x > 7 || y > 7){
        return; 
    }
    frame[y * 8 + x] = color;
}

// Send all 64 colors from memory to the panel
void show_frame(void){
    for(int i = 0; i < NUM_LEDS; i++){
        put_pixel(scale_brightness(frame[i], global_brightness));
    }
    sleep_us(300);
}

// To display image on LED
void show_image_rgb(const uint8_t *rgb){
    for(int i = 0; i < NUM_LEDS; i++){
        put_pixel(urgb_u32(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]));
    }
}