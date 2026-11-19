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
#include "setup.h"
#include "start_word.h"

#define GAME_TASK_PRIORITY        3
#define GAME_TASK_STACK           1024
#define GAME_TEST_FORCE_ONLINE    1   // 1 = skip presence check (testing only)
#define GAME_TEST_AUTOSTART       0   // 1 = skip waiting for START (testing only)
#define GAME_BOOT_WAIT_MS         8000
#define GAME_RESTART_DELAY_MS     5000
#define GAME_ARRANGE_AT_BOOT      1
#define GAME_REARRANGE_EACH_ROUND 0

static GameShared    game;
static QueueHandle_t tap_q;
static volatile bool s_started;
static volatile bool s_arranging;

void game_begin(void) { s_started = true; printf("Game: begin\n"); }

// ---- display & sound helpers (non-blocking MQTT publishes via Mustaqim's API) ----

static void show_on_node(uint8_t n, const char *payload) {
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/node%d/command", MQTT_TOPIC_ROOT, n);
    net_publish(topic, payload, strlen(payload), false);
}

static void show_on_all(const char *payload) {
    for (uint8_t n = 1; n <= NUM_PANELS; n++) {
        show_on_node(n, payload);
    }
}

static void play_match_sound_for_profile(uint8_t profile) {
    if (profile == 0)      buzzer_play(BUZZ_MATCH_STANDARD);
    else if (profile == 1) buzzer_play(BUZZ_MATCH_MARIO);
    else if (profile == 2) buzzer_play(BUZZ_MATCH_PIANO);
    // profile == 3 is Silent
}

// ---- net handlers (run inside net_task: keep tiny, never block) ----

static void on_tap(const char *topic, const uint8_t *p, size_t len) {
    (void)topic;
    if (len != 1) return;

    uint8_t node     = p[0] & 0x0F;
    uint8_t tap_type = (p[0] >> 6) & 0x03; // Unpack top 2 bits (1=single, 2=double, 3=triple)

    if (node < 1 || node > NUM_PANELS) return;

    // --- 1. Toggle DJ Mode on Triple Tap from Panel 1 ---
    if (node == 1 && tap_type == 3) {
        game_shared_toggle_dj_mode(&game);
        GameManager s;
        if (game_shared_snapshot(&game, &s)) {
            printf("Switched modes! DJ mode is now %s\n", s.is_dj_mode ? "ON" : "OFF");
            show_on_all(s.is_dj_mode ? "spade" : "OFF");
        }
        return;
    }

    // --- 2. DJ Mode Routing (separate from Memory Game & Mario song) ---
    GameManager snap;
    if (game_shared_snapshot(&game, &snap) && snap.is_dj_mode) {
        // Double tap in DJ mode cycles the Memory Game sound profile (so 1 Pico can test it)
        if (tap_type == 2) {
            uint8_t new_prof = game_shared_cycle_sound_profile(&game);
            play_match_sound_for_profile(new_prof);
            return;
        }

        if (tap_type == 1) {
            static const buzz_id_t dj_sounds[NUM_PANELS] = {
                BUZZ_DJ_1, BUZZ_DJ_2, BUZZ_DJ_3, BUZZ_DJ_4, BUZZ_DJ_5, BUZZ_DJ_6
            };
            static const char *pad_img[NUM_PANELS] = {
                "diamond", "Clover", "spade", "diamond", "Clover", "spade"
            };
            buzzer_play(dj_sounds[node - 1]);
            show_on_node(node, pad_img[node - 1]);
        }
        return; // Do NOT send DJ taps to the memory game queue
    }

    // --- 3. Sound Profile Toggle in Memory Game Mode (Triple Tap on Panels 2..6) ---
    if (node >= 2 && tap_type == 3) {
        uint8_t new_prof = game_shared_cycle_sound_profile(&game);
        play_match_sound_for_profile(new_prof);
        return;
    }

    // --- 4. Memory Game Routing (passes single & double taps to game_logic.c rules) ---
    if (!s_started && !s_arranging) return;

    tap_evt_t evt = {
        .panel_id  = (uint8_t)(node - 1),
        .from_node = node,
        .tap_type  = tap_type
    };
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

static void send_setup(uint8_t op, uint8_t arg) {
    uint8_t msg[2] = { op, arg };
    net_publish(MQTT_SETUP_TOPIC, msg, sizeof(msg), false);
}

static void arrange_panels(void) {
    uint8_t placed = 0, count = 0;
    xQueueReset(tap_q);
    s_arranging = true;
    send_setup(SETUP_OP_ARRANGE, 0);
    printf("Arrange: tap %d panels in order\n", START_PANELS);
    while (count < START_PANELS) {
        tap_evt_t evt;
        if (xQueueReceive(tap_q, &evt, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        if (evt.tap_type != 1) continue; // only single taps assign panel slots
        uint8_t node = evt.from_node;
        if (node > START_PANELS || (placed & (1u << node))) continue;
        placed |= (uint8_t)(1u << node);
        send_setup(SETUP_OP_SLOT, (uint8_t)((count << 4) | node));
        printf("Arrange: node %d -> slot %d\n", node, count);
        count++;
    }
    s_arranging = false;
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void begin_round(void) {
    s_started = false;
    send_setup(SETUP_OP_START, 0);
    vTaskDelay(pdMS_TO_TICKS(START_HOLD_MS + 500));
    xQueueReset(tap_q);
    game_begin();
}

static void game_task(void *arg) {
    (void)arg;
    tap_evt_t evt;

    uint8_t last_panel_display[NUM_PANELS];
    memset(last_panel_display, 0xFF, sizeof(last_panel_display));

    vTaskDelay(pdMS_TO_TICKS(GAME_BOOT_WAIT_MS));
#if GAME_ARRANGE_AT_BOOT
    arrange_panels();
#endif
    begin_round();

    for (;;) {
        // 1. Process incoming taps through game_logic.c rules (single vs double tap)
        if (xQueueReceive(tap_q, &evt, pdMS_TO_TICKS(250)) == pdTRUE) {
            game_shared_process_tap(&game, evt.panel_id, evt.tap_type);
        }

        // 2. Take a snapshot of the current rules/state
        GameManager snap;
        if (!game_shared_snapshot(&game, &snap)) continue;

        if (snap.is_dj_mode) {
            memset(last_panel_display, 0xFF, sizeof(last_panel_display));
            continue;
        }

        // 3. TRANSLATOR LOOP: Tell each panel what to draw based on the game rules
        for (uint8_t i = 0; i < NUM_PANELS; i++) {
            bool is_revealed = (snap.matched_bitmask & (1 << i)) ||
                               (snap.first_selection == i) ||
                               (snap.second_selection == i);

            uint8_t desired_display = 0xFF;
            if (is_revealed) {
                desired_display = get_card_at_panel(&snap, i);
            }

            if (desired_display != last_panel_display[i]) {
                last_panel_display[i] = desired_display;

                const char *payload = "OFF";
                if (desired_display == 1)      payload = "diamond";
                else if (desired_display == 2) payload = "Clover";
                else if (desired_display == 3) payload = "spade";

                show_on_node(i + 1, payload);
            }
        }

        // 4. Handle match & win events (sound + LED animation hooks via net_publish)
        if (snap.current_state == STATE_EVALUATE_MATCH) {
            uint8_t card1 = get_card_at_panel(&snap, snap.first_selection);
            uint8_t card2 = get_card_at_panel(&snap, snap.second_selection);

            if (card1 == card2) {
                printf("Game Event: Match on panels %u & %u (profile %u)\n",
                       (unsigned)(snap.first_selection + 1),
                       (unsigned)(snap.second_selection + 1),
                       (unsigned)snap.sound_profile);
                play_match_sound_for_profile(snap.sound_profile);

                // Match animation pulse on the two matched panels
                vTaskDelay(pdMS_TO_TICKS(350));
                show_on_node(snap.first_selection + 1, "OFF");
                show_on_node(snap.second_selection + 1, "OFF");
                vTaskDelay(pdMS_TO_TICKS(250));
                const char *match_img = (card1 == 1) ? "diamond" : (card1 == 2) ? "Clover" : "spade";
                show_on_node(snap.first_selection + 1, match_img);
                show_on_node(snap.second_selection + 1, match_img);
                vTaskDelay(pdMS_TO_TICKS(400));
            } else {
                printf("Game Event: Mismatch on panels %u & %u\n",
                       (unsigned)(snap.first_selection + 1),
                       (unsigned)(snap.second_selection + 1));
                buzzer_play(BUZZ_WRONG);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            game_shared_evaluate(&game);
            xQueueReset(tap_q);

        } else if (snap.current_state == STATE_GAME_WON) {
            printf("Game Event: Win! Playing victory animation\n");
            s_started = false;

            // Win animation sweep across all panels via Mustaqim's net_publish interface
            show_on_all("diamond");
            vTaskDelay(pdMS_TO_TICKS(500));
            show_on_all("Clover");
            vTaskDelay(pdMS_TO_TICKS(500));
            show_on_all("spade");
            vTaskDelay(pdMS_TO_TICKS(500));
            show_on_all("OFF");

            vTaskDelay(pdMS_TO_TICKS(GAME_RESTART_DELAY_MS - 1500));
            game_shared_restart(&game);
            xQueueReset(tap_q);
            memset(last_panel_display, 0xFF, sizeof(last_panel_display));
            printf("Game restarted.\n");
#if GAME_REARRANGE_EACH_ROUND
            arrange_panels();
#endif
            begin_round();
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