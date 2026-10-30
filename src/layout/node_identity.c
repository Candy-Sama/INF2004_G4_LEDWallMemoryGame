#include "node_identity.h"

#include <stdio.h>
#include <string.h>
#include "pico/unique_id.h"
#include "grid_config.h"
#include "board_map.h"
#include "node_config.h"

static char s_board_id[IDENTITY_BOARD_ID_LEN];
static volatile int  s_node_id;
static char s_command_topic[IDENTITY_TOPIC_LEN];
static char s_status_topic[IDENTITY_TOPIC_LEN];
static char s_assign_topic[IDENTITY_TOPIC_LEN];
// static char s_peer_topic[IDENTITY_TOPIC_LEN];

bool identity_same_id(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        char x = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char y = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

static bool table_is_valid(void) {
    for (size_t i = 0; i < BOARD_MAP_COUNT; ++i) {
        if (!grid_is_valid_node(BOARD_MAP[i].node_id)) return false;
        for (size_t j = i + 1; j < BOARD_MAP_COUNT; ++j) {
            if (BOARD_MAP[i].node_id == BOARD_MAP[j].node_id) return false;
            if (identity_same_id(BOARD_MAP[i].board_id, BOARD_MAP[j].board_id)) return false;
        }
    }
    return true;
}

bool identity_init(void) {
    s_node_id = 0;
    s_command_topic[0] = s_status_topic[0] = '\0';
    pico_get_unique_board_id_string(s_board_id, sizeof(s_board_id));
    identity_assign_topic_for(s_board_id, s_assign_topic, sizeof(s_assign_topic));

    if (!table_is_valid()) {
        printf("BOARD_MAP is invalid (duplicate or out-of-range entry).\n");
        return false;
    }
    int found = 0;
    for (size_t i = 0; i < BOARD_MAP_COUNT; ++i) {
        if (identity_same_id(s_board_id, BOARD_MAP[i].board_id)) {
            found = BOARD_MAP[i].node_id;
            break;
        }
    }
    if (found) identity_set_node_id(found);
        return true;
}

const char *identity_board_id(void)           { return s_board_id; }
int  identity_node_id(void)                   { return s_node_id; }
bool identity_is_assigned(void)               { return s_node_id != 0; }
bool identity_is_master(void)                 { return s_node_id == 1; }
const char *identity_command_topic(void)      { return s_command_topic; }
const char *identity_status_topic(void)       { return s_status_topic; }
const char *identity_assign_topic(void)       { return s_assign_topic; }
// const char *identity_peer_command_topic(void) { return s_peer_topic; }

bool identity_command_topic_for(int node_id, char *out, size_t out_len) {
    if (!grid_is_valid_node(node_id) || out == NULL) return false;
    int n = snprintf(out, out_len, "%s/node%d/command", MQTT_TOPIC_ROOT, node_id);
    return n > 0 && (size_t)n < out_len;
}

bool identity_assign_topic_for(const char *board_id, char *out, size_t out_len) {
    if (board_id == NULL || out == NULL) return false;
    int n = snprintf(out, out_len, "%s/assign/%s", MQTT_TOPIC_ROOT, board_id);
    return n > 0 && (size_t)n < out_len;
}

void identity_set_node_id(int node_id) {
    if (!grid_is_valid_node(node_id)) return;
    snprintf(s_status_topic, sizeof(s_status_topic),
             "%s/node%d/status", MQTT_TOPIC_ROOT, node_id);
    snprintf(s_command_topic, sizeof(s_command_topic),
             "%s/node%d/command", MQTT_TOPIC_ROOT, node_id);
    s_node_id = node_id;      // set last
}