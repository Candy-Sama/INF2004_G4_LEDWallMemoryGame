#pragma once
#include <stdbool.h>
#include "node_identity.h"

#define GAME_ENGINE_NODE 4
#define IS_GAME_MASTER   (identity_node_id() == GAME_ENGINE_NODE)

bool game_start(void);   // registers its own net handlers
void game_begin(void);   // called when START arrives