#ifndef LEDMATRIX_H
#define LEDMATRIX_H

#include <stdint.h>

#include "hardware/pio.h"

// Shared constants for every LED 
#define LED_PIN 2
#define NUM_LEDS 64

// Sends one color to the panel
static inline void put_pixel(uint32_t pixel_grb){
    pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
}

// Packs red, green and blue into one number
static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
return ((uint32_t)(r) << 8) | ((uint32_t)(g) << 16) | (uint32_t)(b);
}

// To scale the brightness
static inline uint32_t scale_brightness(uint32_t grb_color, uint8_t brightness) {
    uint32_t g = (((grb_color >> 16) & 0xFF) * brightness) / 255;
    uint32_t r = (((grb_color >> 8)  & 0xFF) * brightness) / 255;
    uint32_t b = (( grb_color        & 0xFF) * brightness) / 255;
    return (g << 16) | (r << 8) | b;
}

// Function in ledmatrix.c
void led_init(void);
extern uint32_t frame[NUM_LEDS];
void clear_frame(void);
void set_pixel(int x, int y, uint32_t color);
void show_frame(void);
void set_brightness(uint8_t brightness);
void show_image_rgb(const uint8_t *rgb);

// One 64-pixel WS2812 panel, data on GP2, using PIO0 state machine 0.
void ledmatrix_init(void);
void ledmatrix_fill(uint8_t red, uint8_t green, uint8_t blue);

#endif
