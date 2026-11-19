#include "game_logic.h"
#include <stdio.h>
#include <stdlib.h>

#define LOCK_TIMEOUT pdMS_TO_TICKS(50)
#define ACTIVE_PANELS (NUM_PAIRS * 2)

/* ---------------- pure game logic (no RTOS, call with lock held) ---------------- */

static void set_card(GameManager *g, uint8_t panel, uint8_t value) {
    switch (panel) {
        case 0: g->board_p0 = value; break;
        case 1: g->board_p1 = value; break;
        case 2: g->board_p2 = value; break;
        case 3: g->board_p3 = value; break;
        case 4: g->board_p4 = value; break;
        case 5: g->board_p5 = value; break;
        default: break;
    }
}

uint8_t get_card_at_panel(const GameManager *game, uint8_t panel_id) {
    switch (panel_id) {
        case 0: return game->board_p0;
        case 1: return game->board_p1;
        case 2: return game->board_p2;
        case 3: return game->board_p3;
        case 4: return game->board_p4;
        case 5: return game->board_p5;
        default: return 0;
    }
}

// Deals NUM_PAIRS pairs (values 1..NUM_PAIRS) onto the first ACTIVE_PANELS panels.
// Any leftover panels get value 0 and are locked so they can never be picked.
void game_shuffle_board(GameManager *game) {
    uint8_t deck[ACTIVE_PANELS];
    for (int i = 0; i < ACTIVE_PANELS; i++) deck[i] = (uint8_t)(i / 2 + 1);
    for (int i = ACTIVE_PANELS - 1; i > 0; i--) {          // Fisher-Yates
        int j = rand() % (i + 1);
        uint8_t t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    for (uint8_t p = 0; p < NUM_PANELS; p++) {
        set_card(game, p, p < ACTIVE_PANELS ? deck[p] : 0);
    }
    game->matched_bitmask = 0;
    for (uint8_t p = ACTIVE_PANELS; p < NUM_PANELS; p++) {
        game->matched_bitmask |= (1u << p);                 // unused panels stay locked
    }
}

void game_init(GameManager *game) {
    game->first_selection  = NO_SELECTION;
    game->second_selection = NO_SELECTION;
    game->total_matches    = 0;
    game_shuffle_board(game);                               // also sets matched_bitmask
    game->current_state    = STATE_WAIT_FIRST_CARD;
}

bool game_process_input(GameManager *game, uint8_t panel_id) {
    if (panel_id >= NUM_PANELS) return false;
    if (game->current_state == STATE_PAUSED_DISCONNECT ||
        game->current_state == STATE_GAME_WON) return false;
    if (game->matched_bitmask & (1u << panel_id)) return false;

    if (game->current_state == STATE_WAIT_FIRST_CARD) {
        game->first_selection = panel_id;
        game->current_state   = STATE_WAIT_SECOND_CARD;
        return true;
    }
    if (game->current_state == STATE_WAIT_SECOND_CARD) {
        if (panel_id == game->first_selection) return false;   // FR8: same card rejected
        game->second_selection = panel_id;
        game->current_state    = STATE_EVALUATE_MATCH;
        return true;
    }
    return false;
}

// Rule definition: Single tap (1) selects a card; Double tap (2) is ignored.
bool game_process_tap(GameManager *game, uint8_t panel_id, uint8_t tap_type) {
    if (tap_type == 2) {
        printf("Game Rule: Double tap ignored on panel %u\n", (unsigned)(panel_id + 1));
        return false;
    }
    if (tap_type == 1) {
        bool ok = game_process_input(game, panel_id);
        printf("Game Rule: Single tap on panel %u -> %s\n",
               (unsigned)(panel_id + 1), ok ? "card selected" : "rejected");
        return ok;
    }
    return false;
}

void game_evaluate_match(GameManager *game) {
    if (game->current_state != STATE_EVALUATE_MATCH) return;
    uint8_t card1 = get_card_at_panel(game, game->first_selection);
    uint8_t card2 = get_card_at_panel(game, game->second_selection);

    if (card1 == card2) {
        game->matched_bitmask |= (1u << game->first_selection);
        game->matched_bitmask |= (1u << game->second_selection);
        game->total_matches   += 1;
        if (game->total_matches >= NUM_PAIRS) {
            game->current_state = STATE_GAME_WON;
            return;
        }
    }
    game->first_selection  = NO_SELECTION;
    game->second_selection = NO_SELECTION;
    game->current_state    = STATE_WAIT_FIRST_CARD;
}

/* ---------------- RTOS wrappers ---------------- */

bool game_shared_init(GameShared *g) {
    g->lock = xSemaphoreCreateMutex();
    if (!g->lock) return false;
    g->online_mask         = 0;
    g->state.sound_profile = 0;
    g->state.is_dj_mode    = 0;
    game_init(&g->state);
    g->state.current_state = STATE_PAUSED_DISCONNECT;      // wait for all nodes
    return true;
}

bool game_shared_process_input(GameShared *g, uint8_t panel_id) {
    return game_shared_process_tap(g, panel_id, 1);
}

bool game_shared_process_tap(GameShared *g, uint8_t panel_id, uint8_t tap_type) {
    if (xSemaphoreTake(g->lock, LOCK_TIMEOUT) != pdTRUE) {
        printf("MUTEX: timeout (tap %u)\n", panel_id);
        return false;
    }
    printf("MUTEX: taken (panel %u, type %u)\n", panel_id + 1, tap_type);
    bool ok = game_process_tap(&g->state, panel_id, tap_type);
    xSemaphoreGive(g->lock);
    printf("MUTEX: given (panel %u)%s\n", panel_id + 1, ok ? "" : " [ignored/rejected]");
    return ok;
}

bool game_shared_evaluate(GameShared *g) {
    if (xSemaphoreTake(g->lock, LOCK_TIMEOUT) != pdTRUE) return false;
    game_evaluate_match(&g->state);
    xSemaphoreGive(g->lock);
    return true;
}

bool game_shared_snapshot(GameShared *g, GameManager *out) {
    if (xSemaphoreTake(g->lock, LOCK_TIMEOUT) != pdTRUE) return false;
    *out = g->state;
    xSemaphoreGive(g->lock);
    return true;
}

bool game_shared_restart(GameShared *g) {
    if (xSemaphoreTake(g->lock, LOCK_TIMEOUT) != pdTRUE) return false;
    game_init(&g->state);                                  // keeps sound_profile & is_dj_mode
    if (g->online_mask != ALL_ONLINE) {                    // stay paused if someone is missing
        g->state.current_state = STATE_PAUSED_DISCONNECT;
    }
    xSemaphoreGive(g->lock);
    return true;
}

void game_shared_set_online(GameShared *g, uint8_t node, bool online) {
    if (node >= NUM_PANELS) return;
    if (xSemaphoreTake(g->lock, LOCK_TIMEOUT) != pdTRUE) return;
    if (online) g->online_mask |=  (1u << node);
    else        g->online_mask &= ~(1u << node);

    GameManager *s = &g->state;
    if (g->online_mask != ALL_ONLINE) {
        if (s->current_state != STATE_PAUSED_DISCONNECT &&
            s->current_state != STATE_GAME_WON) {
            s->first_selection  = NO_SELECTION;            // drop a half-made turn
            s->second_selection = NO_SELECTION;
            s->current_state    = STATE_PAUSED_DISCONNECT;
        }
    } else if (s->current_state == STATE_PAUSED_DISCONNECT) {
        s->current_state = STATE_WAIT_FIRST_CARD;
    }
    xSemaphoreGive(g->lock);
}

void game_shared_toggle_dj_mode(GameShared *g) {
    if (xSemaphoreTake(g->lock, LOCK_TIMEOUT) != pdTRUE) return;
    g->state.is_dj_mode = !g->state.is_dj_mode;
    xSemaphoreGive(g->lock);
}

uint8_t game_shared_cycle_sound_profile(GameShared *g) {
    if (xSemaphoreTake(g->lock, LOCK_TIMEOUT) != pdTRUE) return 0;
    g->state.sound_profile = (g->state.sound_profile + 1) % 4;
    uint8_t p = g->state.sound_profile;
    static const char *names[4] = { "Standard", "Mario", "Piano", "Silent" };
    printf("Game Rule: Sound profile toggled -> %u (%s)\n", (unsigned)p, names[p]);
    xSemaphoreGive(g->lock);
    return p;
}

void game_pack_state(const GameManager *g, uint8_t out[2]) {
    out[0] = (g->current_state & 0x7) | ((g->total_matches & 0x3) << 3);
    out[1] = g->matched_bitmask & 0x3F;
}