#ifndef GRID_CONFIG_H
#define GRID_CONFIG_H

#define GRID_COLS 3
#define GRID_ROWS_MAX 4
#define GRID_NODE_MAX (GRID_COLS * GRID_ROWS_MAX)

#include <stdbool.h>

// Ensures the grid has exactly 6 panels to match node_config.h
//_Static_assert(GRID_NODE_COUNT == 6, "Grid must have only 6 panels to match node_config.h");

// Types of content a panel can display
typedef enum {
    // hidden/revealed memory card
    DISPLAY_ROLE_CARD = 0,
    // score/status of game
    DISPLAY_ROLE_SCOREBOARD
} display_role_t;

// Display role assigned to each panel
static const display_role_t GRID_ROLES[GRID_NODE_MAX] = {
    DISPLAY_ROLE_CARD, DISPLAY_ROLE_CARD, DISPLAY_ROLE_CARD,
    DISPLAY_ROLE_CARD, DISPLAY_ROLE_CARD, DISPLAY_ROLE_CARD,
};

// Validation check (is NODE_ID within range)
static inline bool grid_is_valid_node(int node_id) {
    return node_id >= 1 && node_id <= GRID_NODE_MAX;
}

// Row and Column are 0-based, Row 0 is top
// Get the row index of a node, or -1 if the node ID is invalid
static inline int grid_row(int node_id) {
    if (!grid_is_valid_node(node_id)) {
        return -1;
    }

    return (node_id - 1) / GRID_COLS;
}
// Get the column index of a node, or -1 if the node ID is invalid
static inline int grid_col(int node_id) {
    if (!grid_is_valid_node(node_id)) {
        return -1;
    }

    return (node_id - 1) % GRID_COLS;
}

// Find the node at a specific position (e.g. top-left, middle, top-right)
static inline int grid_node_at(int row, int col) {
    if (row < 0 || row >= GRID_ROWS_MAX || col < 0 || col >= GRID_COLS) {
        return 0;
    }
    return row * GRID_COLS + col + 1;
}

// What specific role the node will display
static inline display_role_t grid_role(int node_id) {
    if (!grid_is_valid_node(node_id)) {
        return DISPLAY_ROLE_CARD;
    }

    return GRID_ROLES[node_id - 1];
}

#endif