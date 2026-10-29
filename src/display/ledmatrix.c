#include <stdio.h>
#include "pico/stdlib.h"
#include "ledmatrix.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"

#define LED_PIN 2
#define NUM_LEDS 64

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

void show_image_rgb_rotated(const uint8_t *rgb, uint8_t quarter_turns){
    quarter_turns &= 3;
    for (int y = 0; y < 8; y++){
        for (int x = 0; x < 8; x++){
            int sx, sy;   // source pixel that lands on destination (x, y)
            switch (quarter_turns){
                case 1:  sx = y;     sy = 7 - x; break;   // 90 deg clockwise
                case 2:  sx = 7 - x; sy = 7 - y; break;   // 180 deg
                case 3:  sx = 7 - y; sy = x;     break;   // 270 deg clockwise
                default: sx = x;     sy = y;     break;
            }
            const uint8_t *px = &rgb[(sy * 8 + sx) * 3];
            put_pixel(urgb_u32(px[0], px[1], px[2]));
        }
    }
}

void ledmatrix_init(void) {
    uint offset = pio_add_program(pio0, &ws2812_program);
    ws2812_program_init(pio0, 0, offset, LED_PIN, 800000, false);
}

void ledmatrix_fill(uint8_t red, uint8_t green, uint8_t blue) {
    // WS2812 pixels expect green/red/blue byte order.
    uint32_t colour = ((uint32_t)green << 16) | ((uint32_t)red << 8) | blue;
    for (int pixel = 0; pixel < NUM_LEDS; ++pixel) {
        pio_sm_put_blocking(pio0, 0, colour << 8u);
    }
}
