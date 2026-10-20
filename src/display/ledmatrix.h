#ifndef LEDMATRIX_H
#define LEDMATRIX_H

#include <stdint.h>

// One 64-pixel WS2812 panel, data on GP2, using PIO0 state machine 0.
void ledmatrix_init(void);
void ledmatrix_fill(uint8_t red, uint8_t green, uint8_t blue);

#endif
