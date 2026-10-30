#include "node_registry.h"

#include <string.h>
#include "grid_config.h"
#include "board_map.h"
#include "node_identity.h"

static char s_registry[GRID_NODE_MAX + 1][IDENTITY_BOARD_ID_LEN];   // index = node id

void registry_init(void) {
    memset(s_registry, 0, sizeof s_registry);
    for (size_t i = 0; i < BOARD_MAP_COUNT; ++i) {
        strcpy(s_registry[BOARD_MAP[i].node_id], BOARD_MAP[i].board_id);
    }
}

int registry_assign(const char *board_id) {
    if (board_id == NULL || strlen(board_id) != IDENTITY_BOARD_ID_LEN - 1) return 0;

    for (int id = 1; id <= GRID_NODE_MAX; ++id) {            // known board: same number again
        if (s_registry[id][0] && identity_same_id(s_registry[id], board_id)) return id;
    }
    for (int id = 1; id <= GRID_NODE_MAX; ++id) {            // new board: first free number
        if (!s_registry[id][0]) {
            strcpy(s_registry[id], board_id);
            return id;
        }
    }
    return 0;                                                // full
}