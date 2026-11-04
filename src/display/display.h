#pragma once
#include <stdbool.h>
#include <stdint.h>
bool display_start(void);
void display_show_image(const uint8_t *rgb);
void display_show_colour(uint8_t r, uint8_t g, uint8_t b);
void display_show_own_number(void);
void display_set_slot(uint8_t slot);
void display_set_rotation(uint8_t quarter_turns);
void display_show_start_slice(void);