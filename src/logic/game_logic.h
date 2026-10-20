#ifndef GAME_LOGIC_H
#define GAME_LOGIC_H

#include <stdint.h>
#include <stdbool.h>

#define NUM_PANELS 6
#define NUM_PAIRS 3
#define NO_SELECTION 7 // Used instead of -1 for 3-bit unsigned fields

typedef enum {
    STATE_INIT_BOARD = 0,
    STATE_WAIT_FIRST_CARD = 1,
    STATE_WAIT_SECOND_CARD = 2,
    STATE_EVALUATE_MATCH = 3,
    STATE_GAME_WON = 4,
    STATE_PAUSED_DISCONNECT = 5
} GameState;

// Compressed struct satisfying the Minimum-Bit Change Requirement
typedef struct {
    uint8_t current_state : 3;      
    uint8_t total_matches : 2;      
    uint8_t first_selection : 3;    

    uint8_t second_selection : 3;   
    uint8_t matched_bitmask : 6;    // Packed bitmask for 6 panels 

    uint16_t board_p0 : 2;
    uint16_t board_p1 : 2;
    uint16_t board_p2 : 2;
    uint16_t board_p3 : 2;
    uint16_t board_p4 : 2;
    uint16_t board_p5 : 2;
} GameManager;

void game_init(GameManager* game);
void game_shuffle_board(GameManager* game);
void game_process_input(GameManager* game, uint8_t panel_id);
void game_evaluate_match(GameManager* game);
uint8_t get_card_at_panel(GameManager* game, uint8_t panel_id);

#endif