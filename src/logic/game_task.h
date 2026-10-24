// game_task.h
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "node_identity.h"
#define IS_GAME_MASTER (identity_is_master())
bool game_start(void);
void game_handle_tap_payload(const uint8_t *payload, size_t len);   // called by net_task
void game_publish_output(void);                                      // called by net_task