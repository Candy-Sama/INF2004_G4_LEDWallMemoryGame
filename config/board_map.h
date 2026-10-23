#ifndef BOARD_MAP_H
#define BOARD_MAP_H

typedef struct {
    const char *board_id;   // 16 hex characters, e.g. "E6614103E7123456"
    int node_id;            // GRID_NODE_COUNT
} board_map_t;

static const board_map_t BOARD_MAP[] = {
    { "E66430A64B676F38", 1 },
    { "E66130100F4D6B22", 2 },
    { "E66430A64B63A638", 3 },
    { "E66480454F51382C", 4 },
    { "E66430A64B0F6B38", 5 },
    { "E66430A64B776538", 6 },
};

#define BOARD_MAP_COUNT (sizeof(BOARD_MAP) / sizeof(BOARD_MAP[0]))

#endif