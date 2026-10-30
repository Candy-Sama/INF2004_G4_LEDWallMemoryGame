#ifndef GAME_LOGIC_H
#define GAME_LOGIC_H

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "FreeRTOS.h"
#include "semphr.h"

#define NUM_PANELS   6
#define NUM_PAIRS    3
#define NO_SELECTION 7 // Used instead of -1 for 3-bit unsigned fields
#define ALL_ONLINE   0x3F

typedef enum {
    STATE_INIT_BOARD = 0,
    STATE_WAIT_FIRST_CARD,
    STATE_WAIT_SECOND_CARD,
    STATE_EVALUATE_MATCH,
    STATE_GAME_WON,
    STATE_PAUSED_DISCONNECT
} GameState;

// Compressed struct satisfying the Minimum-Bit Change Requirement
typedef struct {
    uint32_t current_state    : 3;
    uint32_t paused_from      : 3;
    uint32_t total_matches    : 2;
    uint32_t first_selection  : 3;
    uint32_t second_selection : 3;
    uint32_t matched_bitmask  : 6;    // Packed bitmask for 6 panels
    uint32_t board_p0 : 2, board_p1 : 2, board_p2 : 2,
             board_p3 : 2, board_p4 : 2, board_p5 : 2;  // 12 bits
} GameManager; // 3+3+2+3+3+6+12 = 32 bits

static_assert(NUM_PAIRS <= 3, "total_matches is 2 bits");
static_assert(NUM_PANELS <= 6, "matched_bitmask is 6 bits");
static_assert(sizeof(GameManager) == 4, "GameManager must pack into 4 bytes");

// 1-byte (8 bits) tap event, sent by value through the queue
typedef struct {
    uint8_t panel_id  : 3;
    uint8_t from_node : 3;
    uint8_t reserved  : 2;
} tap_evt_t;

typedef struct {
    GameManager         state;
    uint8_t             online_mask; // bit n = node n online (6 bits)
    SemaphoreHandle_t   lock;
} GameShared;

// Logic only, no RTOS (call with the lock held)
void    game_init(GameManager* game);
void    game_shuffle_board(GameManager* game);
bool    game_process_input(GameManager* game, uint8_t panel_id);
void    game_evaluate_match(GameManager* game);
uint8_t get_card_at_panel(const GameManager* game, uint8_t panel_id);

// RTOS wrappers
bool game_shared_init(GameShared* g);
bool game_shared_process_input(GameShared* g, uint8_t panel_id);
bool game_shared_evaluate(GameShared* g);
bool game_shared_snapshot(GameShared* g, GameManager* out);
void game_shared_set_online(GameShared* g, uint8_t node, bool online);

// Wire format (2 bytes)
void game_pack_state(const GameManager* g, uint8_t out[2]);

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