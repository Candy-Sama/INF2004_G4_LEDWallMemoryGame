#include "ledmatrix.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"

#define LED_PIN 2
#define NUM_LEDS 64

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
