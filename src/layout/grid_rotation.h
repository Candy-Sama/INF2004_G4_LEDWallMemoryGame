#ifndef GRID_ROTATION_H
#define GRID_ROTATION_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define GRID_TILE_PX 8
#define GRID_TITLE_BYTES (GRID_TILE_PX * GRID_TILE_PX * 3)

static inline uint8_t grid_image_turns(uint8_t panel_turns){
    return (uint8_t) ((4u - (panel_turns & 3u)) & 3u);
}

static inline void grid_rotate_src(int x, int y, uint8_t turns, int *sx, int *sy){
    switch (turns & 3u){
        case 1: 
            *sx = y;
            *sy = GRID_TILE_PX - 1 - x;
            break;
        case 2:
            *sx = GRID_TILE_PX - 1 - x;
            *sy = GRID_TILE_PX - 1 - y;
            break;
        case 3:
            *sx = GRID_TILE_PX - 1 - y;
            *sy = x;
            break;
        default:
            *sx = x;
            *sy = y;
            break;
    }
}

static inline bool grid_slice_tile(const uint8_t *wide, uint8_t tiles, uint8_t slot, uint8_t out[GRID_TITLE_BYTES]){
    if (wide == NULL || tiles == 0 || slot >= tiles){
        memset(out, 0, GRID_TITLE_BYTES);
        return false;
    }
    const size_t row_bytes = (size_t)tiles * GRID_TILE_PX * 3;
    for (int y = 0; y < GRID_TITLE_BYTES; y++){
        memcpy(&out[y * GRID_TILE_PX * 3], 
            &wide[y * row_bytes + (size_t)slot * GRID_TILE_PX * 3],
            GRID_TILE_PX * 3);
        }
        return true;
}

#endif