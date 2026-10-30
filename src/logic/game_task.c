#include "game_task.h"
#include <stdio.h>
#include <string.h>
#include "game_logic.h"
#include "mqtt_handler.h"
#include "node_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#define GAME_TASK_PRIORITY      3
#define GAME_TASK_STACK         1024
#define GAME_TEST_FORCE_ONLINE  0      // 1 = skip presence check (testing only)
#define GAME_RESTART_DELAY_MS   5000
#define GAME_TEST_AUTOSTART 0   // 1 = skip waiting for START (testing only)

typedef struct { uint8_t bytes[4]; uint8_t len; uint8_t kind; } net_out_t;
enum { OUT_STATE, OUT_REVEAL };

static GameShared    game;
static QueueHandle_t tap_q;       // net_task  -> game_task
static QueueHandle_t net_out_q;   // game_task -> net_task
static volatile bool s_started = false;

static void game_task(void *arg) {
    (void)arg;
    tap_evt_t evt;
    uint8_t last_state[2] = {0xFF, 0xFF};   // publish state only when it changes

    for (;;) {
        // Wake on a tap, or every 250 ms to notice pause/resume changes.
        if (xQueueReceive(tap_q, &evt, pdMS_TO_TICKS(250)) == pdTRUE) {
            game_shared_process_input(&game, evt.panel_id);
        }

        GameManager snap;
        if (!game_shared_snapshot(&game, &snap)) continue;

        net_out_t out = { .kind = OUT_STATE, .len = 2 };
        game_pack_state(&snap, out.bytes);
        if (memcmp(last_state, out.bytes, 2) != 0) {
            memcpy(last_state, out.bytes, 2);
            xQueueSend(net_out_q, &out, 0);
        }

        if (snap.current_state == STATE_EVALUATE_MATCH) {
            net_out_t rev = { .kind = OUT_REVEAL, .len = 1 };
            rev.bytes[0] = (snap.first_selection & 7) | ((snap.second_selection & 7) << 3);
            xQueueSend(net_out_q, &rev, 0);

            vTaskDelay(pdMS_TO_TICKS(1000));      // lock NOT held
            game_shared_evaluate(&game);
            xQueueReset(tap_q);                   // drop taps made during the reveal
        } else if (snap.current_state == STATE_GAME_WON) {
            vTaskDelay(pdMS_TO_TICKS(GAME_RESTART_DELAY_MS));
            game_shared_restart(&game);           // reshuffle and start again
            xQueueReset(tap_q);
            last_state[0] = last_state[1] = 0xFF; // force the new state to publish
            printf("Game restarted.\n");
        }
    }
}

// Tap payload: 1 byte, bits 0-2 = sender's node ID (1..6). Agree this with Afiqah.
void game_handle_tap_payload(const uint8_t *p, size_t len) {
    if (!s_started || len != 1) return;
    uint8_t node = p[0] & 7;
    if (node < 1 || node > NUM_PANELS) return;
    tap_evt_t evt = { .panel_id = (uint8_t)(node - 1), .from_node = node };
    xQueueSend(tap_q, &evt, 0);
}

// Status topics look like <ROOT>/node<N>/status. Returns true if the message was a status message.
bool game_handle_status(const char *topic, const uint8_t *p, size_t len) {
    const char *s = strstr(topic, "/node");
    int node = 0;
    if (!s || sscanf(s, "/node%d/status", &node) != 1) return false;
    if (node < 1 || node > NUM_PANELS) return true;

    size_t n = strlen(MQTT_STATUS_MESSAGE);
    bool online = (len == n && memcmp(p, MQTT_STATUS_MESSAGE, n) == 0);
    printf("Presence: node %d %s\n", node, online ? "online" : "offline");
    game_shared_set_online(&game, (uint8_t)(node - 1), online);
    return true;
}

void game_publish_output(void) {
    net_out_t out;
    while (xQueueReceive(net_out_q, &out, 0) == pdTRUE) {
        const char *topic = (out.kind == OUT_STATE) ? MQTT_STATE_TOPIC : MQTT_REVEAL_TOPIC;
        if (!mqtt_send_ex(topic, out.bytes, out.len, 1, out.kind == OUT_STATE, NULL, NULL)) {
            printf("Game: publish to %s failed.\n", topic);
        }
    }
}

bool game_start(void) {
    tap_q     = xQueueCreate(8, sizeof(tap_evt_t));
    net_out_q = xQueueCreate(8, sizeof(net_out_t));
    if (!tap_q || !net_out_q || !game_shared_init(&game)) return false;
    printf("sizeof(GameManager)=%u\n", (unsigned)sizeof(GameManager));
#if GAME_TEST_FORCE_ONLINE
    for (uint8_t n = 0; n < NUM_PANELS; n++) game_shared_set_online(&game, n, true);
#endif
    return xTaskCreate(game_task, "game", GAME_TASK_STACK, NULL,
                       GAME_TASK_PRIORITY, NULL) == pdPASS;
}

bool game_shared_restart(GameShared* g) {
    if (xSemaphoreTake(g->lock, LOCK_TIMEOUT) != pdTRUE) return false;
    game_init(&g->state);
    if (g->online_mask != ALL_ONLINE) {              // stay paused if someone is missing
        g->state.paused_from   = STATE_WAIT_FIRST_CARD;
        g->state.current_state = STATE_PAUSED_DISCONNECT;
    }
    xSemaphoreGive(g->lock);
    return true;
}