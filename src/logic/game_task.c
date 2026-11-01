#include "game_task.h"
#include <stdio.h>
#include <string.h>
#include "game_logic.h"
#include "net.h"
#include "node_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#define GAME_TASK_PRIORITY      3
#define GAME_TASK_STACK         1024
#define GAME_TEST_FORCE_ONLINE  0   // 1 = skip presence check (testing only)
#define GAME_TEST_AUTOSTART     0   // 1 = skip waiting for START (testing only)
#define GAME_RESTART_DELAY_MS   5000

static GameShared        game;
static QueueHandle_t     tap_q;
static volatile bool     s_started;

void game_begin(void) { s_started = true; printf("Game: begin\n"); }

// ---- net handlers (run inside net_task: keep tiny, never block) ----

static void on_tap(const char *topic, const uint8_t *p, size_t len) {
    (void)topic;
    if (!s_started || len != 1) return;
    uint8_t node = p[0] & 7;                         // sender's node ID 1..6
    if (node < 1 || node > NUM_PANELS) return;
    tap_evt_t evt = { .panel_id = (uint8_t)(node - 1), .from_node = node };
    xQueueSend(tap_q, &evt, 0);
}

static void on_status(const char *topic, const uint8_t *p, size_t len) {
    const char *s = strstr(topic, "/node");
    int node = 0;
    if (!s || sscanf(s, "/node%d/status", &node) != 1) return;
    if (node < 1 || node > NUM_PANELS) return;
    size_t n = strlen(MQTT_STATUS_MESSAGE);
    bool online = (len == n && memcmp(p, MQTT_STATUS_MESSAGE, n) == 0);
    printf("Presence: node %d %s\n", node, online ? "online" : "offline");
    game_shared_set_online(&game, (uint8_t)(node - 1), online);
}

// ---- task ----

static void game_task(void *arg) {
    (void)arg;
    tap_evt_t evt;
    uint8_t last_state[2] = {0xFF, 0xFF};

    for (;;) {
        if (xQueueReceive(tap_q, &evt, pdMS_TO_TICKS(250)) == pdTRUE) {
            game_shared_process_input(&game, evt.panel_id);
        }

        GameManager snap;
        if (!game_shared_snapshot(&game, &snap)) continue;

        uint8_t state[2];
        game_pack_state(&snap, state);
        if (memcmp(last_state, state, 2) != 0) {
            memcpy(last_state, state, 2);
            net_publish(MQTT_STATE_TOPIC, state, 2, true);
        }

        if (snap.current_state == STATE_EVALUATE_MATCH) {
            uint8_t rev = (snap.first_selection & 7) | ((snap.second_selection & 7) << 3);
            net_publish(MQTT_REVEAL_TOPIC, &rev, 1, false);

            vTaskDelay(pdMS_TO_TICKS(1000));          // lock NOT held
            game_shared_evaluate(&game);
            xQueueReset(tap_q);
        } else if (snap.current_state == STATE_GAME_WON) {
            vTaskDelay(pdMS_TO_TICKS(GAME_RESTART_DELAY_MS));
            game_shared_restart(&game);
            xQueueReset(tap_q);
            last_state[0] = last_state[1] = 0xFF;
            printf("Game restarted.\n");
        }
    }
}

bool game_start(void) {
    tap_q = xQueueCreate(8, sizeof(tap_evt_t));
    if (!tap_q || !game_shared_init(&game)) return false;
    s_started = GAME_TEST_AUTOSTART;
    printf("sizeof(GameManager)=%u\n", (unsigned)sizeof(GameManager));
#if GAME_TEST_FORCE_ONLINE
    for (uint8_t n = 0; n < NUM_PANELS; n++) game_shared_set_online(&game, n, true);
#endif
    if (!net_register_handler(MQTT_TAP_TOPIC, on_tap)) return false;
    if (!net_register_handler(MQTT_TOPIC_ROOT "/+/status", on_status)) return false;
    return xTaskCreate(game_task, "game", GAME_TASK_STACK, NULL,
                       GAME_TASK_PRIORITY, NULL) == pdPASS;
}