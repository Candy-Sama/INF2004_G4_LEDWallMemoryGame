#include "game_logic.h"
#include <stdio.h>
#include <stdlib.h>

#define LOCK_TIMEOUT pdMS_TO_TICKS(50)

/* ---------------- pure game logic (no RTOS, call with lock held) ---------------- */

void game_shuffle_board(GameManager* game) {
    uint8_t deck[NUM_PANELS] = {1, 1, 2, 2, 3, 3};

    // Fisher-Yates shuffle
    for (int i = NUM_PANELS - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        uint8_t temp = deck[i];
        deck[i] = deck[j];
        deck[j] = temp;
    }

    game->board_p0 = deck[0];
    game->board_p1 = deck[1];
    game->board_p2 = deck[2];
    game->board_p3 = deck[3];
    game->board_p4 = deck[4];
    game->board_p5 = deck[5];
}

void game_init(GameManager* game) {
    game->first_selection  = NO_SELECTION;
    game->second_selection = NO_SELECTION;
    game->total_matches    = 0;
    game->matched_bitmask  = 0;
    game->paused_from      = STATE_WAIT_FIRST_CARD;
    game_shuffle_board(game);
    game->current_state    = STATE_WAIT_FIRST_CARD;
}

uint8_t get_card_at_panel(const GameManager* game, uint8_t panel_id) {
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

bool game_process_input(GameManager* game, uint8_t panel_id) {
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
        if (panel_id == game->first_selection) return false;   // FR8
        game->second_selection = panel_id;
        game->current_state    = STATE_EVALUATE_MATCH;
        return true;
    }
    return false;
}

void game_evaluate_match(GameManager* game) {
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

bool game_shared_init(GameShared* g) {
    g->lock = xSemaphoreCreateMutex();
    if (!g->lock) return false;
    g->online_mask = 0;
    game_init(&g->state);
    g->state.paused_from   = STATE_WAIT_FIRST_CARD;
    g->state.current_state = STATE_PAUSED_DISCONNECT;   // wait for all nodes
    return true;
}

bool game_shared_process_input(GameShared* g, uint8_t panel_id) {
    if (xSemaphoreTake(g->lock, LOCK_TIMEOUT) != pdTRUE) {
        printf("MUTEX: timeout (tap %u)\n", panel_id);
        return false;
    }
    printf("MUTEX: taken (tap %u)\n", panel_id);
    bool ok = game_process_input(&g->state, panel_id);
    xSemaphoreGive(g->lock);
    printf("MUTEX: given (tap %u)%s\n", panel_id, ok ? "" : " [rejected]");
    return ok;
}

bool game_shared_evaluate(GameShared* g) {
    if (xSemaphoreTake(g->lock, LOCK_TIMEOUT) != pdTRUE) return false;
    game_evaluate_match(&g->state);
    xSemaphoreGive(g->lock);
    return true;
}

bool game_shared_snapshot(GameShared* g, GameManager* out) {
    if (xSemaphoreTake(g->lock, LOCK_TIMEOUT) != pdTRUE) return false;
    *out = g->state;
    xSemaphoreGive(g->lock);
    return true;
}

void game_shared_set_online(GameShared* g, uint8_t node, bool online) {
    if (node >= NUM_PANELS) return;
    if (xSemaphoreTake(g->lock, LOCK_TIMEOUT) != pdTRUE) return;

    if (online) g->online_mask |=  (1u << node);
    else        g->online_mask &= ~(1u << node);

    GameManager* s = &g->state;
    if (g->online_mask != ALL_ONLINE) {
        if (s->current_state != STATE_PAUSED_DISCONNECT &&
            s->current_state != STATE_GAME_WON) {
            s->paused_from   = s->current_state;
            s->current_state = STATE_PAUSED_DISCONNECT;
        }
    } else if (s->current_state == STATE_PAUSED_DISCONNECT) {
        s->current_state = s->paused_from;
    }
    xSemaphoreGive(g->lock);
}

void game_pack_state(const GameManager* g, uint8_t out[2]) {
    out[0] = (g->current_state & 0x7) | ((g->total_matches & 0x3) << 3);
    out[1] = g->matched_bitmask & 0x3F;
}