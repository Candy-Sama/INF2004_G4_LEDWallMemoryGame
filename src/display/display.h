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

// Fart / DJ mode burst & inter-panel color transport animations
void display_anim_burst(uint8_t node, uint8_t kit);
void display_anim_tx_out(uint8_t from_node, uint8_t kit, bool to_right);
void display_anim_tx_pass(uint8_t from_node, uint8_t kit, bool to_right);
void display_anim_tx_in(uint8_t from_node, uint8_t target_node, uint8_t kit, bool to_right);

// Spliced Mario relay animation across N x 8 panels
void display_anim_mario(uint8_t slice_idx, uint8_t total_slices, uint32_t duration_ms);

// Scrolling wide strip (8 px tall, row-major RGB). start_master_us is on the SHARED clock.
void display_play_strip(const uint8_t *strip, uint16_t cols, uint16_t ms_per_col, uint32_t start_master_us);

// START word scrolling across total_slices panels (this panel draws slice slice_idx)
void display_anim_start_word(uint8_t slice_idx, uint8_t total_slices, uint16_t ms_per_col);