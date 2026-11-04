#include "game_task.h"
#include <stdio.h>
#include <string.h>
#include "game_logic.h"
#include "net.h"
#include "node_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "buzzer.h"

#define GAME_TASK_PRIORITY      3
#define GAME_TASK_STACK         1024
#define GAME_TEST_FORCE_ONLINE  1   // 1 = skip presence check (testing only)
#define GAME_TEST_AUTOSTART     1   // 1 = skip waiting for START (testing only)
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
    
    // Track what is currently showing on each screen so we only send updates when things change
    uint8_t last_panel_display[NUM_PANELS];
    memset(last_panel_display, 0xFF, sizeof(last_panel_display));

    for (;;) {
        // 1. Process incoming taps
        if (xQueueReceive(tap_q, &evt, pdMS_TO_TICKS(250)) == pdTRUE) {
            game_shared_process_input(&game, evt.panel_id);
        }

        // 2. Take a snapshot of the current rules/state
        GameManager snap;
        if (!game_shared_snapshot(&game, &snap)) continue;

        // 3. TRANSLATOR LOOP: Tell each panel what to draw based on the game rules
        for (uint8_t i = 0; i < NUM_PANELS; i++) {
            // A card should be visible if it's permanently matched, OR if it's currently selected
            bool is_revealed = (snap.matched_bitmask & (1 << i)) || 
                               (snap.first_selection == i) || 
                               (snap.second_selection == i);
            
            // 0xFF means face-down/OFF. The pairs are 0, 1, and 2.
            uint8_t desired_display = 0xFF; 
            if (is_revealed) {
                desired_display = get_card_at_panel(&snap, i); 
            }

            // If the screen needs to change, send the MQTT command
            if (desired_display != last_panel_display[i]) {
                last_panel_display[i] = desired_display;
                
                char target_topic[64];
                snprintf(target_topic, sizeof(target_topic), "%s/node%d/command", MQTT_TOPIC_ROOT, i + 1);
                
                const char* payload = "OFF";
                if (desired_display == 0) {
                    payload = "diamond";
                } else if (desired_display == 1) {
                    payload = "Clover";
                } else if (desired_display == 2) {
                    payload = "diamond"; // TEMP FIX: Reusing Diamond for Pair 3 until you add a 3rd image!
                }
                
                net_publish(target_topic, payload, strlen(payload), false);
            }
        } 
            

        // 4. Handle matches and restarts
        if (snap.current_state == STATE_EVALUATE_MATCH) {
            
            // Check if the two selected cards match
            uint8_t card1 = get_card_at_panel(&snap, snap.first_selection);
            uint8_t card2 = get_card_at_panel(&snap, snap.second_selection);
            
            if (card1 == card2) {
                buzzer_play(BUZZ_JINGLE); // Match: play good jingle
            } else {
                buzzer_play(BUZZ_WRONG);  // Mismatch: play bad jingle before turning OFF
            }

            // Wait 1 second so the players can see both cards before they flip back
            vTaskDelay(pdMS_TO_TICKS(1000));          // lock NOT held
            game_shared_evaluate(&game);
            xQueueReset(tap_q);
            
        } else if (snap.current_state == STATE_GAME_WON) {
        } else if (snap.current_state == STATE_GAME_WON) {
            vTaskDelay(pdMS_TO_TICKS(GAME_RESTART_DELAY_MS));
            game_shared_restart(&game);
            xQueueReset(tap_q);
            memset(last_panel_display, 0xFF, sizeof(last_panel_display)); // Reset tracking array
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
   // if (!net_register_handler(MQTT_TOPIC_ROOT "/+/status", on_status)) return false;
    return xTaskCreate(game_task, "game", GAME_TASK_STACK, NULL,
                       GAME_TASK_PRIORITY, NULL) == pdPASS;
}