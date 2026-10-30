#ifndef NODE_REGISTRY_H
#define NODE_REGISTRY_H

// Master only. Seeds the registry from BOARD_MAP so known boards keep their numbers.
void registry_init(void);

// Returns the node id for this board: the same number if it's already known,
// otherwise the first free number. Returns 0 if the grid is full.
int registry_assign(const char *board_id);

#endif