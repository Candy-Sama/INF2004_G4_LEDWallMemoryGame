// game_task.h
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "node_identity.h"

// Node 4 runs the game engine. Confirm this with the team.
#define GAME_ENGINE_NODE 4
#define IS_GAME_MASTER   (identity_node_id() == GAME_ENGINE_NODE)

bool game_start(void);

// Called by net_task (game engine node only).
void game_handle_tap_payload(const uint8_t *payload, size_t len);
bool game_handle_status(const char *topic, const uint8_t *payload, size_t len); // true = consumed
void game_publish_output(void);
bool game_shared_restart(GameShared* g);
void game_begin(void);   // called when START arrives (engine node)