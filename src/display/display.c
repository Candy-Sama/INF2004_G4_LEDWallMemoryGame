// display.c  (Farhah's module + DJ/Fart burst, transport & spliced wide-screen Mario show)
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "node_identity.h"
#include "ledmatrix.h"
#include "imagedata.h"
#include "start_word.h"
#include "faults.h"
#include "wire_format.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "clock_sync.h"

#define DISPLAY_TASK_PRIORITY 4
#define DISPLAY_TASK_STACK    768
#define START_BLUE            5
#define SLOT_UNKNOWN          0xFF
#define ANIM_FRAME_MS         30
#define ANIM_MAX_BRIGHTNESS   25   // 20% of 255 max LED channel brightness
#ifndef WALL_PANELS
#define WALL_PANELS        4      // number of panels in the wall (change if you add boards)
#endif
#define FRAME_TOGGLE_GPIO  21     // logic-analyser pin: flips on every column step

typedef enum {
    CMD_IMAGE,
    CMD_COLOUR,
    CMD_REDRAW,
    CMD_START,
    CMD_ANIM,
    CMD_STRIP
} cmd_kind_t;

typedef enum {
    ANIM_BURST = 0,
    ANIM_TX_OUT,
    ANIM_TX_PASS,
    ANIM_TX_IN,
    ANIM_MARIO,
    ANIM_START_WORD
} anim_type_t;

typedef struct {
    cmd_kind_t     kind;
    const uint8_t *image;
    uint8_t        r, g, b;
    anim_type_t    anim_type;
    uint8_t        from_node;
    uint8_t        to_node;
    uint8_t        kit;          // 0 = DJ neon palette, 1 = Fart toxic palette
    bool           to_right;
    uint8_t        slice_idx;    // 0 .. total_slices - 1
    uint8_t        total_slices; // number of active relay panels
    uint32_t       duration_ms;  // full relay playback duration in ms
    uint16_t       strip_cols;       // CMD_STRIP: strip width in columns
    uint16_t       ms_per_col;       // CMD_STRIP: scroll speed
    uint32_t       start_master_us;  // CMD_STRIP: shared-clock start time
} display_cmd_t;

static QueueHandle_t    display_q;
static volatile uint8_t s_rotation;
static volatile uint8_t s_slot = SLOT_UNKNOWN;

// Saturated per-node palettes (~13-14% peak before wave scaling)
static const uint8_t k_dj_palette[6][3] = {
    { 34,  2,  3 }, // Node 1: Electric Red
    {  0, 28, 34 }, // Node 2: Neon Cyan
    { 32,  0, 32 }, // Node 3: Hot Magenta
    {  2, 35,  5 }, // Node 4: Laser Green
    { 35, 21,  0 }, // Node 5: Club Gold
    {  3,  9, 36 }  // Node 6: Royal Blue
};

static const uint8_t k_fart_palette[6][3] = {
    { 15, 35,  0 }, // Node 1: Toxic Lime
    { 26, 34,  0 }, // Node 2: Acid Chartreuse
    { 35, 24,  0 }, // Node 3: Mustard Gas
    {  0, 32, 15 }, // Node 4: Swamp Teal
    { 36, 10,  0 }, // Node 5: Biohazard Orange
    { 23,  3, 32 }  // Node 6: Murky Violet
};

static void send_cmd(display_cmd_t cmd) {
    if (xQueueSend(display_q, &cmd, 0) == pdPASS) {
        fault_clear(FAULT_DISPLAY);
    } else {
        fault_set(FAULT_DISPLAY);
    }
}

void display_show_colour(uint8_t r, uint8_t g, uint8_t b) {
    display_cmd_t cmd = { .kind = CMD_COLOUR, .r = r, .g = g, .b = b };
    send_cmd(cmd);
}

void display_show_image(const uint8_t *rgb) {
    display_cmd_t cmd = { .kind = CMD_IMAGE, .image = rgb };
    send_cmd(cmd);
}

void display_show_start_slice(void) {
    display_cmd_t cmd = { .kind = CMD_START };
    send_cmd(cmd);
}

void display_set_slot(uint8_t slot) { s_slot = slot; }

void display_set_rotation(uint8_t quarter_turns) {
    quarter_turns &= 3;
    if (s_rotation == quarter_turns) return;
    s_rotation = quarter_turns;
    display_cmd_t cmd = { .kind = CMD_REDRAW };
    send_cmd(cmd);
}

void display_anim_burst(uint8_t node, uint8_t kit) {
    display_cmd_t cmd = {
        .kind = CMD_ANIM, .anim_type = ANIM_BURST,
        .from_node = node, .to_node = node, .kit = kit
    };
    send_cmd(cmd);
}

void display_anim_tx_out(uint8_t from_node, uint8_t kit, bool to_right) {
    display_cmd_t cmd = {
        .kind = CMD_ANIM, .anim_type = ANIM_TX_OUT,
        .from_node = from_node, .to_node = from_node, .kit = kit, .to_right = to_right
    };
    send_cmd(cmd);
}

void display_anim_tx_pass(uint8_t from_node, uint8_t kit, bool to_right) {
    display_cmd_t cmd = {
        .kind = CMD_ANIM, .anim_type = ANIM_TX_PASS,
        .from_node = from_node, .to_node = from_node, .kit = kit, .to_right = to_right
    };
    send_cmd(cmd);
}

void display_anim_tx_in(uint8_t from_node, uint8_t target_node, uint8_t kit, bool to_right) {
    display_cmd_t cmd = {
        .kind = CMD_ANIM, .anim_type = ANIM_TX_IN,
        .from_node = from_node, .to_node = target_node, .kit = kit, .to_right = to_right
    };
    send_cmd(cmd);
}

void display_anim_mario(uint8_t slice_idx, uint8_t total_slices, uint32_t duration_ms) {
    display_cmd_t cmd = {
        .kind         = CMD_ANIM,
        .anim_type    = ANIM_MARIO,
        .slice_idx    = slice_idx,
        .total_slices = (total_slices > 0) ? total_slices : 1,
        .duration_ms  = (duration_ms > 0) ? duration_ms : 1000
    };
    send_cmd(cmd);
}

void display_anim_start_word(uint8_t slice_idx, uint8_t total_slices, uint16_t ms_per_col) {
    display_cmd_t cmd = {
        .kind         = CMD_ANIM,
        .anim_type    = ANIM_START_WORD,
        .slice_idx    = slice_idx,
        .total_slices = (total_slices > 0) ? total_slices : 1,
        .ms_per_col   = (ms_per_col > 0) ? ms_per_col : 80
    };
    send_cmd(cmd);
}

void display_play_strip(const uint8_t *strip, uint16_t cols, uint16_t ms_per_col, uint32_t start_master_us) {
    display_cmd_t cmd = {
        .kind = CMD_STRIP, .image = strip, .strip_cols = cols,
        .ms_per_col = ms_per_col, .start_master_us = start_master_us
    };
    send_cmd(cmd);
}

void display_show_own_number(void) {
    static const uint8_t *const number_cards[] = {
        Number1Card_bin, Number2Card_bin, Number3Card_bin,
        Number4Card_bin, Number5Card_bin, Number6Card_bin,
    };
    int id = identity_node_id();
    if (id >= 1 && id <= (int)(sizeof(number_cards) / sizeof(number_cards[0]))) {
        display_show_image(number_cards[id - 1]);
    } else {
        display_show_colour(12, 6, 0);
    }
}

static uint8_t current_slot(void) {
    uint8_t slot = s_slot;
    if (slot != SLOT_UNKNOWN) return slot;
    int id = identity_node_id();
    return (id >= 1 && id <= START_PANELS) ? (uint8_t)(id - 1) : 0;
}

static void get_node_rgb(uint8_t node, uint8_t kit, uint8_t out[3]) {
    uint8_t idx = (node >= 1 && node <= 6) ? (uint8_t)(node - 1) : 0;
    const uint8_t (*pal)[3] = (kit == 1) ? k_fart_palette : k_dj_palette;
    out[0] = pal[idx][0];
    out[1] = pal[idx][1];
    out[2] = pal[idx][2];
}

// Hard-caps every animation channel at ANIM_MAX_BRIGHTNESS
static inline uint8_t clamp_anim(int v) {
    if (v < 0) return 0;
    if (v > ANIM_MAX_BRIGHTNESS) return ANIM_MAX_BRIGHTNESS;
    return (uint8_t)v;
}

static inline void set_px(uint8_t tile[192], int r, int c, uint8_t cr, uint8_t cg, uint8_t cb) {
    if (r < 0 || r >= 8 || c < 0 || c >= 8) return;
    int off = (r * 8 + c) * 3;
    tile[off + 0] = clamp_anim(cr);
    tile[off + 1] = clamp_anim(cg);
    tile[off + 2] = clamp_anim(cb);
}

// Clips shared (N*8) x 8 world coordinates onto this panel's 8x8 puzzle-piece viewport
static inline void set_world_px(uint8_t tile[192], int world_x0, int r, int wx,
                                uint8_t cr, uint8_t cg, uint8_t cb) {
    int lc = wx - world_x0;
    if (lc < 0 || lc >= 8) return;
    set_px(tile, r, lc, cr, cg, cb);
}

// Linear radial distance from center (3.5, 3.5): 1 at center 2x2 up to 10 at corners
static inline int center_dist(int r, int c) {
    int dx = abs((c * 2) - 7); // 1, 3, 5, 7
    int dy = abs((r * 2) - 7); // 1, 3, 5, 7
    int mx = (dx > dy) ? dx : dy;
    int mn = (dx > dy) ? dy : dx;
    return mx + (mn >> 1);     // 1..10
}

// Full-panel center-out burst (~15% shockwave peak) + soft idle breathing (~3-5%)
static void render_center_burst(uint8_t tile[192], const uint8_t rgb[3],
                                uint32_t elapsed_ms, uint32_t expand_ms) {
    bool expanding = (elapsed_ms < expand_ms);
    int wave_r = expanding ? (1 + (int)((elapsed_ms * 11) / expand_ms)) : 12;

    int phase = (int)((elapsed_ms / 20) % 100);
    int breath = (phase < 50) ? (phase * 2) : ((100 - phase) * 2); // 0..100
    int idle_scale = 20 + (breath * 15) / 100;                     // 20%..35% of palette (~3-5% raw)

    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int dist = center_dist(r, c);
            int scale = 0;
            int sparkle = 0;

            if (expanding) {
                int diff = abs(dist - wave_r);
                if (diff <= 2) {
                    scale = 100 - diff * 16;
                    sparkle = (2 - diff) * 2;
                } else if (dist < wave_r) {
                    scale = 55 - (wave_r - dist) * 2;
                    if (scale < 38) scale = 38;
                }
            } else {
                scale = idle_scale + (11 - dist);
            }

            int off = (r * 8 + c) * 3;
            tile[off + 0] = clamp_anim((rgb[0] * scale) / 100 + sparkle);
            tile[off + 1] = clamp_anim((rgb[1] * scale) / 100 + sparkle);
            tile[off + 2] = clamp_anim((rgb[2] * scale) / 100 + sparkle);
        }
    }
}

// Draws a clean, high-contrast 4x4 Super Mushroom (red cap + white dots + tan base)
static void draw_simple_mushroom(uint8_t tile[192], int world_x0, int top_r, int left_wx) {
    static const char k_mush[4][4] = {
        { '.', 'R', 'R', '.' },
        { 'R', 'W', 'W', 'R' },
        { 'R', 'R', 'R', 'R' },
        { '.', 'T', 'T', '.' }
    };
    for (int dr = 0; dr < 4; dr++) {
        for (int dc = 0; dc < 4; dc++) {
            char ch = k_mush[dr][dc];
            if (ch == '.') continue;
            if (ch == 'R')      set_world_px(tile, world_x0, top_r + dr, left_wx + dc, 48,  2,  2);
            else if (ch == 'W') set_world_px(tile, world_x0, top_r + dr, left_wx + dc, 45, 45, 45);
            else                set_world_px(tile, world_x0, top_r + dr, left_wx + dc, 40, 22,  8);
        }
    }
}

// Draws a warm, unmistakable gold star (4x4) without blowing out to white
static void draw_simple_star(uint8_t tile[192], int world_x0, int top_r, int left_wx) {
    static const char k_star[4][4] = {
        { '.', 'Y', 'Y', '.' },
        { 'Y', 'G', 'G', 'Y' },
        { 'Y', 'Y', 'Y', 'Y' },
        { 'Y', '.', '.', 'Y' }
    };
    for (int dr = 0; dr < 4; dr++) {
        for (int dc = 0; dc < 4; dc++) {
            char ch = k_star[dr][dc];
            if (ch == '.') continue;
            if (ch == 'G') set_world_px(tile, world_x0, top_r + dr, left_wx + dc, 50, 34, 0);
            else           set_world_px(tile, world_x0, top_r + dr, left_wx + dc, 42, 24, 0);
        }
    }
}

// Spliced Mario animation across all (total_slices * 8) columns with bottom dancing lines & fireworks finale
static bool render_mario_slice(uint8_t tile[192], const display_cmd_t *anim, uint32_t elapsed_ms) {
    if (elapsed_ms >= anim->duration_ms) return false;

    int total_slices = (anim->total_slices > 0) ? (int)anim->total_slices : 1;
    int W            = total_slices * 8;
    int world_x0     = (int)anim->slice_idx * 8;
    uint32_t dur     = (anim->duration_ms > 0) ? anim->duration_ms : 1000;
    uint32_t pct     = (elapsed_ms * 1000) / dur;

    // Dancing equalizer lines along bottom rows (r = 6..7) across all connected panels
    static const uint8_t k_eq_palette[4][3] = {
        { 25,  2,  2 }, // Mario Red
        { 25, 18,  0 }, // Coin Gold
        {  0, 24,  6 }, // Pipe Green
        {  0, 18, 25 }  // Sky Cyan
    };
    for (int c = 0; c < 8; c++) {
        int wx = world_x0 + c;
        int w_phase = (int)((wx * 3 + (elapsed_ms / 55)) % 12);
        int bar_h   = (w_phase < 6) ? (w_phase / 2) : ((12 - w_phase) / 2); // 0..3
        const uint8_t *ec = k_eq_palette[((wx / 2) + (elapsed_ms / 300)) % 4];

        set_px(tile, 7, c, ec[0], ec[1], ec[2]);
        if (bar_h >= 2) {
            set_px(tile, 6, c,
                   (uint8_t)(ec[0] / 2), (uint8_t)(ec[1] / 2), (uint8_t)(ec[2] / 2));
        }
    }

    // Act 1 (0..45%): Bouncing Red & White Mushroom traveling across all connected panels
    if (pct < 450) {
        int sub = (int)((pct * 1000) / 450);
        int left_wx = -4 + (sub * (W + 4)) / 1000;

        int b_phase = (int)((elapsed_ms / 50) % 12);
        int bounce  = (b_phase < 6) ? b_phase : (12 - b_phase);
        int top_r   = 2 - (bounce * 2) / 6;

        draw_simple_mushroom(tile, world_x0, top_r, left_wx);
        return true;
    }

    // Act 2 (45..90%): Bouncing Gold Star traveling across all connected panels (no green flag)
    if (pct < 900) {
        int sub = (int)(((pct - 450) * 1000) / 450);
        int left_wx = -4 + (sub * (W + 4)) / 1000;

        int s_phase = (int)((elapsed_ms / 45) % 12);
        int bounce  = (s_phase < 6) ? s_phase : (12 - s_phase);
        int top_r   = 2 - (bounce * 2) / 6;

        draw_simple_star(tile, world_x0, top_r, left_wx);
        return true;
    }

    // Finale (90..100%): Multi-panel fireworks across the W x 8 canvas
    int fw_sub = (int)(((pct - 900) * 1000) / 100);
    int radius = 1 + (fw_sub * 9) / 1000;

    static const uint8_t k_fw_rgb[3][3] = {
        { 50, 40,  0 }, // Gold
        { 50,  4,  8 }, // Red
        {  0, 44, 50 }  // Cyan
    };
    int centers[3] = { W / 4, W / 2, (3 * W) / 4 };

    for (int f = 0; f < 3; f++) {
        int f_rad = radius - f;
        if (f_rad < 0) continue;
        const uint8_t *fc = k_fw_rgb[f];
        int fade = 100 - (f_rad * 7);
        if (fade < 20) fade = 20;

        for (int r = 0; r < 6; r++) {
            int dy = abs(r - 2);
            for (int c = 0; c < 8; c++) {
                int wx = world_x0 + c;
                int dx = abs(wx - centers[f]);
                int d  = (dx > dy ? dx : dy) + ((dx > dy ? dy : dx) >> 1);
                if (abs(d - f_rad) <= 1) {
                    set_px(tile, r, c,
                           (uint8_t)((fc[0] * fade) / 100),
                           (uint8_t)((fc[1] * fade) / 100),
                           (uint8_t)((fc[2] * fade) / 100));
                }
            }
        }
    }

    return true;
}

// START word scrolling across total_slices panels. This panel draws only its own 8 columns.
static bool render_start_slice(uint8_t tile[192], const display_cmd_t *anim, uint32_t elapsed_ms) {
    uint32_t per = anim->ms_per_col ? anim->ms_per_col : 80;
    int W   = (int)anim->total_slices * 8;
    int off = (int)(elapsed_ms / per) - W;       // strip column at the wall's left edge
    if (off >= START_STRIP_COLS) return false;   // word has left the wall
    int x0 = (int)anim->slice_idx * 8;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int sc = x0 + x + off;
            if (sc < 0 || sc >= START_STRIP_COLS) continue;
            memcpy(&tile[(y * 8 + x) * 3], &START_strip[(y * START_STRIP_COLS + sc) * 3], 3);
        }
    }
    return true;
}

// Renders one 8x8 RGB frame into tile[192]. Returns true while animation is still active.
static bool render_anim_frame(uint8_t tile[192], const display_cmd_t *anim, uint32_t elapsed_ms) {
    memset(tile, 0, 192);

    if (anim->anim_type == ANIM_MARIO) {
        return render_mario_slice(tile, anim, elapsed_ms);
    }
    if (anim->anim_type == ANIM_START_WORD) {
        return render_start_slice(tile, anim, elapsed_ms);
    }

    uint8_t c_from[3], c_to[3];
    get_node_rgb(anim->from_node, anim->kit, c_from);
    get_node_rgb(anim->to_node,   anim->kit, c_to);

    if (anim->anim_type == ANIM_BURST) {
        render_center_burst(tile, c_to, elapsed_ms, 650);
        return true;
    }

    if (anim->anim_type == ANIM_TX_OUT) {
        if (elapsed_ms >= 220) return false;
        int prog = (int)((elapsed_ms * 22) / 220);
        int wave_pos = anim->to_right ? (14 + prog) : (18 - prog);

        for (int r = 0; r < 8; r++) {
            int row_weight = 100 - abs((r * 2) - 7) * 5;
            for (int c = 0; c < 8; c++) {
                int col_pos = c * 4 + 2;
                int diff = abs(col_pos - wave_pos);
                if (diff >= 12) continue;

                int scale = (((12 - diff) * 95) / 12) * row_weight / 100;
                int sparkle = (diff < 3) ? (3 - diff) : 0;
                int off = (r * 8 + c) * 3;
                tile[off + 0] = clamp_anim((c_from[0] * scale) / 100 + sparkle);
                tile[off + 1] = clamp_anim((c_from[1] * scale) / 100 + sparkle);
                tile[off + 2] = clamp_anim((c_from[2] * scale) / 100 + sparkle);
            }
        }
        return true;
    }

    if (anim->anim_type == ANIM_TX_PASS) {
        if (elapsed_ms >= 240) return false;
        int prog = (int)((elapsed_ms * 40) / 240);
        int wave_pos = anim->to_right ? (-4 + prog) : (36 - prog);

        for (int r = 0; r < 8; r++) {
            int row_weight = 100 - abs((r * 2) - 7) * 6;
            for (int c = 0; c < 8; c++) {
                int col_pos = c * 4 + 2;
                int diff = abs(col_pos - wave_pos);
                if (diff >= 11) continue;

                int scale = (((11 - diff) * 95) / 11) * row_weight / 100;
                int sparkle = (diff < 3) ? (3 - diff) : 0;
                int off = (r * 8 + c) * 3;
                tile[off + 0] = clamp_anim((c_from[0] * scale) / 100 + sparkle);
                tile[off + 1] = clamp_anim((c_from[1] * scale) / 100 + sparkle);
                tile[off + 2] = clamp_anim((c_from[2] * scale) / 100 + sparkle);
            }
        }
        return true;
    }

    // ANIM_TX_IN: 0..170 ms incoming streak to center, then full-panel center-out morph burst
    if (elapsed_ms < 170) {
        int prog = (int)((elapsed_ms * 20) / 170);
        int wave_pos = anim->to_right ? (-4 + prog) : (36 - prog);

        for (int r = 0; r < 8; r++) {
            int row_weight = 100 - abs((r * 2) - 7) * 5;
            for (int c = 0; c < 8; c++) {
                int col_pos = c * 4 + 2;
                int diff = abs(col_pos - wave_pos);
                if (diff >= 10) continue;

                int scale = (((10 - diff) * 95) / 10) * row_weight / 100;
                int sparkle = (diff < 3) ? (3 - diff) : 0;
                int off = (r * 8 + c) * 3;
                tile[off + 0] = clamp_anim((c_from[0] * scale) / 100 + sparkle);
                tile[off + 1] = clamp_anim((c_from[1] * scale) / 100 + sparkle);
                tile[off + 2] = clamp_anim((c_from[2] * scale) / 100 + sparkle);
            }
        }
        return true;
    }

    uint32_t burst_ms = elapsed_ms - 170;
    const uint32_t expand_ms = 600;
    int mix = (burst_ms < expand_ms) ? (int)((burst_ms * 100) / expand_ms) : 100;

    uint8_t cur[3];
    cur[0] = (uint8_t)((c_from[0] * (100 - mix) + c_to[0] * mix) / 100);
    cur[1] = (uint8_t)((c_from[1] * (100 - mix) + c_to[1] * mix) / 100);
    cur[2] = (uint8_t)((c_from[2] * (100 - mix) + c_to[2] * mix) / 100);

    render_center_burst(tile, cur, burst_ms, expand_ms);
    return true;
}

// Slot for the strip. Does not use current_slot() because that clamps to START_PANELS.
static uint8_t strip_slot(void) {
    if (s_slot != SLOT_UNKNOWN) return s_slot;
    int id = identity_node_id();
    return (id >= 1) ? (uint8_t)(id - 1) : 0;
}

// Cut this panel's 8 columns out of the wide strip. 'off' = strip column at the wall's left edge.
static void strip_tile(uint8_t tile[192], const uint8_t *strip, int cols, int off) {
    bool vertical = (s_rotation & 1) != 0;
    int place = (s_rotation == 0 || s_rotation == 3) ? strip_slot()
                                                     : (WALL_PANELS - 1 - strip_slot());
    memset(tile, 0, 192);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int along  = place * 8 + (vertical ? y : x);
            int across = vertical ? x : y;
            int sc = along + off;
            if (sc < 0 || sc >= cols) continue;          // outside the strip = black
            memcpy(&tile[(y * 8 + x) * 3], &strip[(across * cols + sc) * 3], 3);
        }
    }
}

// Which column step are we on, according to the SHARED clock?
// Returns -1 before the start (or if unsynced). *us_to_next = microseconds until the next step.
static int32_t strip_step(const display_cmd_t *s, uint32_t *us_to_next) {
    uint32_t m;
    *us_to_next = 20000;
    if (!clock_sync_now_master_us(&m)) return -1;
    int32_t since = (int32_t)(m - s->start_master_us);
    if (since < 0) { *us_to_next = (uint32_t)(-since); return -1; }
    uint32_t per = (uint32_t)s->ms_per_col * 1000u;
    *us_to_next = per - ((uint32_t)since % per);
    return (int32_t)((uint32_t)since / per);
}

static void display_task(void *arg) {
    (void)arg;
    ledmatrix_init();

    static uint8_t tile[192];
    static display_cmd_t strip;
    display_cmd_t cmd = {0}, incoming;
    bool have_cmd = false, start_on = false, anim_active = false, strip_active = false;
    TickType_t start_end = 0, anim_start = 0;
    int32_t last_step = -1;

    for (;;) {
        TickType_t wait = (start_on || anim_active)
                        ? pdMS_TO_TICKS(ANIM_FRAME_MS)
                        : pdMS_TO_TICKS(500);

        // Strip playback: sleep until ~1.5 ms before the next column boundary of the shared clock
        uint32_t t0 = time_us_32(), us_next = 0;
        if (strip_active) {
            (void)strip_step(&strip, &us_next);
            uint32_t sleep_us = (us_next > 2000) ? (us_next - 1500) : 0;
            if (sleep_us > 100000) sleep_us = 100000;
            wait = pdMS_TO_TICKS(sleep_us / 1000);
        }

        BaseType_t got = xQueueReceive(display_q, &incoming, wait);
        if (got == pdTRUE) {
            if (incoming.kind == CMD_STRIP) {
                strip        = incoming;
                strip_active = true;
                last_step    = -1;
                start_on     = false;
                anim_active  = false;
                have_cmd     = false;
            } else if (incoming.kind == CMD_START) {
                strip_active = false;
                start_on  = true;
                start_end = xTaskGetTickCount() + pdMS_TO_TICKS(START_HOLD_MS);
            } else if (incoming.kind == CMD_ANIM) {
                strip_active = false;
                cmd         = incoming;
                have_cmd    = true;
                anim_active = true;
                anim_start  = xTaskGetTickCount();
            } else if (incoming.kind != CMD_REDRAW) {
                strip_active = false;
                cmd         = incoming;
                have_cmd    = true;
                anim_active = false;
            }
        }

        if (start_on && (int32_t)(xTaskGetTickCount() - start_end) >= 0) {
            start_on = false;
        }

        if (start_on) {
            start_word_tile(tile, current_slot(), s_rotation, START_BLUE);
            taskENTER_CRITICAL();
            show_image_rgb_rotated(tile, s_rotation);
            taskEXIT_CRITICAL();
            continue;
        }

        if (strip_active) {
            if (got == pdFALSE) {                         // timed out: a boundary is close
                uint32_t until = t0 + us_next;
                if ((uint32_t)(until - time_us_32()) < 3000) {
                    while ((int32_t)(time_us_32() - until) < 0) tight_loop_contents();
                }
            }
            uint32_t dummy;
            int32_t step = strip_step(&strip, &dummy);
            if (step >= 0 && step != last_step) {
                int off = step - WALL_PANELS * 8;         // step 0: word fully off the right edge
                if (off >= (int)strip.strip_cols) {       // word has left the wall: finished
                    strip_active = false;
                    taskENTER_CRITICAL();
                    ledmatrix_fill(0, 0, 0);
                    taskEXIT_CRITICAL();
                    continue;
                }
                strip_tile(tile, strip.image, strip.strip_cols, off);
                gpio_xor_mask(1u << FRAME_TOGGLE_GPIO);   // evidence: one edge per frame change
                taskENTER_CRITICAL();
                show_image_rgb_rotated(tile, s_rotation);
                taskEXIT_CRITICAL();
                last_step = step;
            }
            continue;
        }

        if (!have_cmd) continue;

        if (cmd.kind == CMD_ANIM) {
            uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - anim_start) * portTICK_PERIOD_MS);
            bool keep_going = render_anim_frame(tile, &cmd, elapsed_ms);
            taskENTER_CRITICAL();
            show_image_rgb_rotated(tile, s_rotation);
            taskEXIT_CRITICAL();
            if (!keep_going) {
                anim_active = false;
                have_cmd    = false;
            }
            continue;
        }

        taskENTER_CRITICAL();
        if (cmd.kind == CMD_IMAGE && cmd.image) {
            show_image_rgb_rotated(cmd.image, s_rotation);
        } else {
            ledmatrix_fill(cmd.r, cmd.g, cmd.b);
        }
        taskEXIT_CRITICAL();
    }
}

bool display_start(void) {
    gpio_init(FRAME_TOGGLE_GPIO);
    gpio_set_dir(FRAME_TOGGLE_GPIO, GPIO_OUT);
    display_q = xQueueCreate(8, sizeof(display_cmd_t));
    if (!display_q) return false;
    return xTaskCreate(display_task, "display", DISPLAY_TASK_STACK, NULL,
                       DISPLAY_TASK_PRIORITY, NULL) == pdPASS;
}

// Strong override of the weak hook in clock_sync.c. Called at the scheduled shared-clock instant.
void anim_on_play(uint8_t anim_id, uint8_t slot) {
    (void)slot;   // we use s_slot (set during arrange) instead
    if (anim_id == 0) {                                   // anim A = scrolling START
        display_play_strip(START_strip, START_STRIP_COLS, 80, clock_sync_play_start_us());
    } else {                                              // B..D keep the old colour flash
        static const uint8_t c[4][3] = { {12,0,0}, {0,12,0}, {0,0,12}, {12,10,0} };
        display_show_colour(c[anim_id & 3][0], c[anim_id & 3][1], c[anim_id & 3][2]);
    }
}