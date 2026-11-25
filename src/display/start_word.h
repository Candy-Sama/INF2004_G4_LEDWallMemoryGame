#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define START_PANELS   4
#define START_HOLD_MS  5000

static const uint8_t START_FONT_BIG[5][7] = {
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},   // S
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},   // T
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},   // A
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},   // R
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},   // T
};

static const uint8_t START_FONT_SMALL[5][5] = {
    {7, 4, 7, 1, 7},   // S
    {7, 2, 2, 2, 2},   // T
    {2, 5, 7, 5, 5},   // A
    {6, 5, 6, 5, 5},   // R
    {7, 2, 2, 2, 2},   // T
};

static inline bool start_pixel_lit(int cx, int cy, bool vertical) {
    if (!vertical) {
        int c = cx - 1, row = cy - 1;
        if (c < 0 || c >= 29 || row < 0 || row > 6) return false;
        int letter = c / 6, within = c % 6;
        if (within == 5) return false;
        return (START_FONT_BIG[letter][row] >> (4 - within)) & 1;
    }
    int r = cy - 1, col = cx - 2;
    if (r < 0 || r >= 30 || col < 0 || col > 2) return false;
    int letter = r / 6, within = r % 6;
    if (within == 5) return false;
    return (START_FONT_SMALL[letter][within] >> (2 - col)) & 1;
}

static inline void start_word_tile(uint8_t rgb[192], uint8_t slot, uint8_t turns,
                                   uint8_t blue) {
    turns &= 3;
    if (slot >= START_PANELS) slot = START_PANELS - 1;
    bool vertical = (turns & 1) != 0;
    int place = (turns == 0 || turns == 3) ? slot : (START_PANELS - 1 - slot);
    memset(rgb, 0, 192);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int cx = vertical ? x : place * 8 + x;
            int cy = vertical ? place * 8 + y : y;
            if (start_pixel_lit(cx, cy, vertical)) rgb[(y * 8 + x) * 3 + 2] = blue;
        }
    }
}