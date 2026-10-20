#include "game_logic.h"
#include <stdlib.h> 

void game_init(GameManager* game) {
    game->first_selection = NO_SELECTION;
    game->second_selection = NO_SELECTION;
    game->total_matches = 0;
    game->matched_bitmask = 0; // Resets all 6 panels instantly
    
    game_shuffle_board(game);
    game->current_state = STATE_WAIT_FIRST_CARD;
}

void game_shuffle_board(GameManager* game) {
    uint8_t deck[NUM_PANELS] = {1, 1, 2, 2, 3, 3};
    
    // Fisher-Yates shuffle, randomiser
    for (int i = NUM_PANELS - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        uint8_t temp = deck[i];
        deck[i] = deck[j];
        deck[j] = temp;
    }
    
    // Map the shuffled deck to the individual bit-fields
    game->board_p0 = deck[0];
    game->board_p1 = deck[1];
    game->board_p2 = deck[2];
    game->board_p3 = deck[3];
    game->board_p4 = deck[4];
    game->board_p5 = deck[5];
}

// Helper to read the packed bit-field board
uint8_t get_card_at_panel(GameManager* game, uint8_t panel_id) {
    switch(panel_id) {
        case 0: return game->board_p0;
        case 1: return game->board_p1;
        case 2: return game->board_p2;
        case 3: return game->board_p3;
        case 4: return game->board_p4;
        case 5: return game->board_p5;
        default: return 0;
    }
}

void game_process_input(GameManager* game, uint8_t panel_id) {
    // 1. Ignore all inputs if the game is paused or already over
    if (game->current_state == STATE_PAUSED_DISCONNECT || game->current_state == STATE_GAME_WON) {
        return; 
    }

    // 2. Ignore the tap if this panel has already been permanently matched
    if (game->matched_bitmask & (1 << panel_id)) {
        return; 
    }

    // 3. Process the first card selection
    if (game->current_state == STATE_WAIT_FIRST_CARD) {
        game->first_selection = panel_id;
        game->current_state = STATE_WAIT_SECOND_CARD;
    }
    // 4. Process the second card selection
    else if (game->current_state == STATE_WAIT_SECOND_CARD) {
        // FR8: Ignore same-card double tap. 
        if (panel_id == game->first_selection) {
            return;
        }

        game->second_selection = panel_id;
        game->current_state = STATE_EVALUATE_MATCH;
    }
}

void game_evaluate_match(GameManager* game) {
    if (game->current_state != STATE_EVALUATE_MATCH) {
        return;
    }

    uint8_t card1 = get_card_at_panel(game, game->first_selection);
    uint8_t card2 = get_card_at_panel(game, game->second_selection);

    if (card1 == card2) {
        // Match found! Lock both panels using bitwise OR
        game->matched_bitmask |= (1 << game->first_selection);
        game->matched_bitmask |= (1 << game->second_selection);
        
        game->total_matches += 1;

        // Check win condition (FR7)
        if (game->total_matches >= NUM_PAIRS) {
            game->current_state = STATE_GAME_WON;
            return; 
        }
    }

    // Reset selections and wait for the next turn
    game->first_selection = NO_SELECTION;
    game->second_selection = NO_SELECTION;
    game->current_state = STATE_WAIT_FIRST_CARD;
}