#ifndef GAME_LOGIC_H
#define GAME_LOGIC_H

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "FreeRTOS.h"
#include "semphr.h"

#define NUM_PANELS    4
#define NUM_PAIRS     2                              // 2 pairs = 4 panels (set to 3 for 6 panels)
#define ACTIVE_PANELS (NUM_PAIRS * 2)
#define NO_SELECTION  7                              // sentinel for 3-bit unsigned selection fields
#define ALL_ONLINE    ((1u << ACTIVE_PANELS) - 1u)   // 0x0F for 4 panels, 0x3F for 6 panels

typedef enum {
    STATE_INIT_BOARD = 0,
    STATE_WAIT_FIRST_CARD,
    STATE_WAIT_SECOND_CARD,
    STATE_EVALUATE_MATCH,
    STATE_GAME_WON,
    STATE_PAUSED_DISCONNECT
} GameState;

// Packed game state (Minimum-Bit Change Requirement): exactly 32 bits.
typedef struct {
    uint32_t current_state    : 3;
    uint32_t total_matches    : 2;
    uint32_t first_selection  : 3;
    uint32_t second_selection : 3;
    uint32_t matched_bitmask  : 6;   // bit n = slot n locked
    uint32_t sound_profile    : 2;   // 0=Standard, 1=Mario, 2=Piano, 3=Silent
    uint32_t is_dj_mode       : 1;   // 0 = memory game, 1 = DJ/Fart soundboard
    uint32_t board_p0 : 2, board_p1 : 2, board_p2 : 2,
             board_p3 : 2, board_p4 : 2, board_p5 : 2;   // card value per slot (1..3)
} GameManager;                       // 3+2+3+3+6+2+1+12 = 32 bits

static_assert(NUM_PAIRS >= 1 && NUM_PAIRS <= 3, "total_matches is 2 bits, cards are 2 bits");
static_assert(ACTIVE_PANELS <= NUM_PANELS, "not enough panels for the pairs");
static_assert(NUM_PANELS <= 6, "matched_bitmask is 6 bits");
static_assert(sizeof(GameManager) == 4, "GameManager must pack into 4 bytes");

// 1-byte tap event, sent by value through the queue
typedef struct {
    uint8_t panel_id  : 3;   // 0..5
    uint8_t from_node : 3;   // 1..6
    uint8_t tap_type  : 2;   // 1 = single, 2 = double, 3 = triple
} tap_evt_t;

typedef struct {
    GameManager       state;
    uint8_t           online_mask;   // bit n = node n+1 online
    SemaphoreHandle_t lock;
} GameShared;

// Pure logic, no RTOS (call with the lock held)
void    game_init(GameManager *game);
void    game_shuffle_board(GameManager *game);
bool    game_process_input(GameManager *game, uint8_t panel_id);
bool    game_process_tap(GameManager *game, uint8_t panel_id, uint8_t tap_type);
void    game_evaluate_match(GameManager *game);
uint8_t get_card_at_panel(const GameManager *game, uint8_t panel_id);

// RTOS wrappers (take the mutex)
bool    game_shared_init(GameShared *g);
bool    game_shared_process_input(GameShared *g, uint8_t panel_id);
bool    game_shared_process_tap(GameShared *g, uint8_t panel_id, uint8_t tap_type);
bool    game_shared_evaluate(GameShared *g);
bool    game_shared_snapshot(GameShared *g, GameManager *out);
bool    game_shared_restart(GameShared *g);
void    game_shared_clear_selection(GameShared *g);
void    game_shared_set_online(GameShared *g, uint8_t node, bool online);
void    game_shared_toggle_dj_mode(GameShared *g);
uint8_t game_shared_cycle_sound_profile(GameShared *g);
uint8_t game_shared_get_online_mask(GameShared *g);
uint8_t game_shared_online_count(GameShared *g);

// Wire format (2 bytes)
void game_pack_state(const GameManager *g, uint8_t out[2]);

#ifndef MQTT_TAP_TOPIC
#define MQTT_TAP_TOPIC     "se04/game/tap"
#endif
#ifndef MQTT_STATE_TOPIC
#define MQTT_STATE_TOPIC   "se04/game/state"
#endif
#ifndef MQTT_REVEAL_TOPIC
#define MQTT_REVEAL_TOPIC  "se04/game/reveal"
#endif

#endif /* GAME_LOGIC_H */