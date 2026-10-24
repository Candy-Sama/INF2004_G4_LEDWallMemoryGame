#pragma once
#include <stdbool.h>
#include <stdint.h>
bool display_start(void);
void display_show_image(const uint8_t *rgb);
void display_show_colour(uint8_t r, uint8_t g, uint8_t b);
void display_show_own_number(void);