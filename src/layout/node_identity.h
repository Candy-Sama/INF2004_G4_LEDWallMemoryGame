#ifndef NODE_IDENTITY_H
#define NODE_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

#define IDENTITY_BOARD_ID_LEN 17  // 16 hex chars + terminator
#define IDENTITY_TOPIC_LEN 48

// true if this board is in BOARD_MAP with valid panel number and table has no duplicates
bool identity_init(void);

const char *identity_board_id(void);
int identity_node_id(void); // node id 1-6 or 0 is unknown
bool identity_is_master(void); // demo purpose: panel 1

// This panel listens for commands (subscribe to it).
const char *identity_command_topic(void);
// This panel announces online/offline
const char *identity_status_topic(void);

// Address of the target panel we send commands to (publish to it).
// const char *identity_peer_command_topic(void);

// Writes the command topic of ANY panel (1..6) into out.
// Returns false if the panel number is invalid or out is too small.
bool identity_command_topic_for(int node_id, char *out, size_t out_len);

#endif