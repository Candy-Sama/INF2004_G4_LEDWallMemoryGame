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
#define GAME_TEST_FORCE_ONLINE    0   // 0 = count actual connected Picos via /status
#define GAME_TEST_AUTOSTART       0
#define GAME_BOOT_WAIT_MS         8000
#define GAME_RESTART_DELAY_MS     5000
#define GAME_ARRANGE_AT_BOOT      1
#define GAME_REARRANGE_EACH_ROUND 0
#define TAP_TIMEOUT_MS            20000

// Display command payloads handled in net.c
#define ANIM_CMD_MATCH_PULSE  "OFF"
#define ANIM_CMD_WIN_1        "win"
#define ANIM_CMD_WIN_2        "OFF"
#define ANIM_CMD_WIN_3        "win"
#define ANIM_CMD_GAMEOVER     "lose"

static GameShared          game;
static QueueHandle_t       tap_q;
static volatile bool       s_started;
static volatile bool       s_arranging;
static volatile bool       s_play_mario_relay;
static volatile bool       s_use_fart_kit = true;
static volatile uint8_t    s_mario_trigger_node = 1;
static volatile TickType_t s_last_tap_tick;
static volatile uint8_t    s_dj_tap_node    = 0;
static volatile bool       s_dj_tap_pending = false;
static uint8_t             s_prev_dj_node   = 0;
static uint8_t             s_node_to_slot[NUM_PANELS] = {0, 1, 2, 3, 4, 5};
static uint8_t             s_slot_to_node[NUM_PANELS] = {1, 2, 3, 4, 5, 6};

void game_begin(void) {
    s_last_tap_tick = xTaskGetTickCount();
    s_started = true;
    printf("Game: begin\n");
}

// ---- display & sound helpers ----

static void show_on_node(uint8_t n, const char *payload) {
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/node%d/command", MQTT_TOPIC_ROOT, n);
    net_publish(topic, payload, strlen(payload), false);
}

static void show_on_all(const char *payload) {
    uint8_t mask = game_shared_get_online_mask(&game);
    for (uint8_t n = 1; n <= NUM_PANELS; n++) {
        if (mask == 0 || (mask & (1u << (n - 1)))) {
            show_on_node(n, payload);
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
}

static void play_match_sound_for_profile(uint8_t profile) {
    if (profile == 0)      buzzer_play(BUZZ_MATCH_STANDARD);
    else if (profile == 1) buzzer_play(BUZZ_MATCH_MARIO);
    else if (profile == 2) buzzer_play(BUZZ_MATCH_PIANO);
}

// Existing DJ/Fart node-to-node color transport animation (unchanged)
static void run_dj_transport_anim(uint8_t target_node) {
    char cmd[24];
    uint8_t kit = s_use_fart_kit ? 1 : 0; // 1 = Fart palette, 0 = DJ palette

    // First tap (or tapping the same panel twice): radial color burst only
    if (s_prev_dj_node == 0 || s_prev_dj_node == target_node) {
        snprintf(cmd, sizeof(cmd), "burst:%u:%u", (unsigned)target_node, (unsigned)kit);
        show_on_node(target_node, cmd);
        s_prev_dj_node = target_node;
        return;
    }

    uint8_t from_node = s_prev_dj_node;
    int8_t  from_slot = (int8_t)s_node_to_slot[from_node - 1];
    int8_t  to_slot   = (int8_t)s_node_to_slot[target_node - 1];
    int8_t  step      = (to_slot > from_slot) ? 1 : -1;
    char    dir       = (step > 0) ? 'R' : 'L';

    // 1. Sweep color out of the previous panel
    snprintf(cmd, sizeof(cmd), "tx_out:%u:%u:%c", (unsigned)from_node, (unsigned)kit, dir);
    show_on_node(from_node, cmd);
    vTaskDelay(pdMS_TO_TICKS(120));

    // 2. Glide across any intermediate panels in physical slot order
    for (int8_t s = from_slot + step; s != to_slot; s += step) {
        GameManager snap;
        if (!game_shared_snapshot(&game, &snap) || !snap.is_dj_mode) return;

        uint8_t mid_node = s_slot_to_node[s];
        snprintf(cmd, sizeof(cmd), "tx_pass:%u:%u:%c", (unsigned)from_node, (unsigned)kit, dir);
        show_on_node(mid_node, cmd);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    // 3. Splash incoming color into target panel and morph into its signature burst
    snprintf(cmd, sizeof(cmd), "tx_in:%u:%u:%u:%c",
             (unsigned)from_node, (unsigned)target_node, (unsigned)kit, dir);
    show_on_node(target_node, cmd);

    s_prev_dj_node = target_node;
}

// Plays wide-screen Mario animation across ALL connected panels while splitting buzzer audio
static void run_mario_relay(void) {
    uint8_t mask = game_shared_get_online_mask(&game);
    uint8_t connected[NUM_PANELS];
    uint8_t count = 0;

    // Collect connected nodes in physical left-to-right slot order
    for (uint8_t slot = 0; slot < NUM_PANELS; slot++) {
        uint8_t node = s_slot_to_node[slot];
        if (node >= 1 && node <= NUM_PANELS && (mask & (1u << (node - 1)))) {
            connected[count++] = node;
        }
    }
    if (count == 0) {
        connected[0] = s_mario_trigger_node;
        count = 1;
    }

    uint8_t  total_notes = buzzer_mario_note_count();
    uint32_t total_ms    = buzzer_mario_slice_ms(0, total_notes);
    uint8_t  note_idx    = 0;
    uint32_t elapsed_ms  = 0;

    printf("Mario Relay: wide-screen animation + %lu ms song across %u node(s)\n",
           (unsigned long)total_ms, (unsigned)count);

    // 1. Start the synchronized wide-screen Super Star animation across ALL connected panels
    for (uint8_t k = 0; k < count; k++) {
        char anim_cmd[32];
        snprintf(anim_cmd, sizeof(anim_cmd), "mario_all:%u:%u:%lu",
                 (unsigned)k, (unsigned)count, (unsigned long)total_ms);
        show_on_node(connected[k], anim_cmd);
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    // 2. Pass the buzzer melody slice-by-slice across each connected node
    for (uint8_t k = 0; k < count && note_idx < total_notes; k++) {
        GameManager snap;
        if (!game_shared_snapshot(&game, &snap) || !snap.is_dj_mode) break;

        uint32_t target_ms = ((uint32_t)(k + 1) * total_ms) / count;
        uint8_t start = note_idx;

        if (k == count - 1) {
            note_idx = total_notes;
        } else {
            while (note_idx < total_notes && elapsed_ms < target_ms) {
                elapsed_ms += buzzer_mario_slice_ms(note_idx, (uint8_t)(note_idx + 1));
                note_idx++;
            }
        }

        uint32_t slice_ms = buzzer_mario_slice_ms(start, note_idx);
        char audio_cmd[16];
        snprintf(audio_cmd, sizeof(audio_cmd), "ms:%u:%u", (unsigned)start, (unsigned)note_idx);
        show_on_node(connected[k], audio_cmd);

        printf("Mario Relay: Node %u playing notes %u..%u (%lu ms)\n",
               (unsigned)connected[k], (unsigned)start, (unsigned)note_idx, (unsigned long)slice_ms);

        uint32_t waited = 0;
        while (waited < slice_ms) {
            uint32_t step = (slice_ms - waited > 100) ? 100 : (slice_ms - waited);
            vTaskDelay(pdMS_TO_TICKS(step));
            waited += step;
            if (!game_shared_snapshot(&game, &snap) || !snap.is_dj_mode) break;
        }
    }

    // 3. Restore active mode icon only if still in DJ/Fart mode
    GameManager end_snap;
    if (game_shared_snapshot(&game, &end_snap) && end_snap.is_dj_mode) {
        show_on_all(s_use_fart_kit ? "spade" : "Clover");
    }

    s_prev_dj_node   = 0;
    s_dj_tap_pending = false;
    s_last_tap_tick  = xTaskGetTickCount();
}

// ---- net handlers (run inside net_task: keep tiny, never block) ----

static void on_tap(const char *topic, const uint8_t *p, size_t len) {
    (void)topic;
    if (len != 1) return;

    uint8_t node     = p[0] & 0x0F;
    uint8_t tap_type = (p[0] >> 6) & 0x03;

    if (node < 1 || node > NUM_PANELS) return;

    // --- 1. Cycle Memory Game -> Fart Mode -> Drum Mode on Triple Tap from Panel 1 ---
    if (node == 1 && tap_type == 3) {
        s_play_mario_relay = false;
        s_dj_tap_pending   = false;
        s_prev_dj_node     = 0;

        GameManager s;
        if (game_shared_snapshot(&game, &s)) {
            if (!s.is_dj_mode) {
                s_use_fart_kit = true;
                game_shared_toggle_dj_mode(&game); // Memory -> Fart Mode
                printf("Switched modes! FART mode is now ON\n");
                show_on_all("spade");
            } else if (s_use_fart_kit) {
                s_use_fart_kit = false;            // Fart Mode -> Drum Mode
                printf("Switched modes! DJ DRUM mode is now ON\n");
                show_on_all("Clover");
            } else {
                game_shared_toggle_dj_mode(&game); // Drum Mode -> Memory Game
                printf("Switched modes! Memory Game mode is now ON\n");
                s_last_tap_tick = xTaskGetTickCount();
                show_on_all("OFF");
            }
        }
        return;
    }

    // --- 2. DJ / Fart Mode Routing ---
    GameManager snap;
    if (game_shared_snapshot(&game, &snap) && snap.is_dj_mode) {
        if (tap_type == 1) {
            static const char *dj_cmds[NUM_PANELS] = {
                "dj1", "dj2", "dj3", "dj4", "dj5", "dj6"
            };
            static const char *fart_cmds[NUM_PANELS] = {
                "fart1", "fart2", "fart3", "fart4", "fart5", "fart6"
            };
            show_on_node(node, s_use_fart_kit ? fart_cmds[node - 1] : dj_cmds[node - 1]);
            s_dj_tap_node    = node;
            s_dj_tap_pending = true;
        } else if (tap_type == 2) {
            s_mario_trigger_node = node;
            s_play_mario_relay   = true;
        } else if (node == 2 && tap_type == 3) {
            s_use_fart_kit   = !s_use_fart_kit;
            s_dj_tap_pending = false;
            s_prev_dj_node   = 0;
            printf("DJ Sound Kit: %s\n", s_use_fart_kit ? "FART" : "DRUMS");
            show_on_all(s_use_fart_kit ? "spade" : "Clover");
        }
        return;
    }

    // --- 3. Sound Profile Toggle in Memory Game Mode (Triple Tap on Panels 2..6) ---
    if (node >= 2 && tap_type == 3) {
        uint8_t new_prof = game_shared_cycle_sound_profile(&game);
        play_match_sound_for_profile(new_prof);
        s_last_tap_tick = xTaskGetTickCount();
        return;
    }

    // --- 4. Memory Game Routing ---
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

    uint8_t idx = (uint8_t)(node - 1);
    static TickType_t last_print[NUM_PANELS];
    static bool printed_once[NUM_PANELS];
    static bool last_online[NUM_PANELS];
    TickType_t now = xTaskGetTickCount();

    if (!printed_once[idx] || last_online[idx] != online ||
        (now - last_print[idx]) >= pdMS_TO_TICKS(30000)) {
        printf("Presence: node %d %s\n", node, online ? "online" : "offline");
        last_print[idx]   = now;
        last_online[idx]  = online;
        printed_once[idx] = true;
    }

    game_shared_set_online(&game, idx, online);
}

// ---- setup & main task ----

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
        if (s_play_mario_relay) {
            s_play_mario_relay = false;
            run_mario_relay();
        }
        if (s_dj_tap_pending) {
            uint8_t target = s_dj_tap_node;
            s_dj_tap_pending = false;
            run_dj_transport_anim(target);
        }
        tap_evt_t evt;
        if (xQueueReceive(tap_q, &evt, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        if (evt.tap_type != 1) continue;
        uint8_t node = evt.from_node;
        if (node > START_PANELS || (placed & (1u << node))) continue;
        placed |= (uint8_t)(1u << node);
        s_node_to_slot[node - 1] = count;
        s_slot_to_node[count]    = node;
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
    GameState prev_state = STATE_INIT_BOARD;
    s_last_tap_tick = xTaskGetTickCount();

    vTaskDelay(pdMS_TO_TICKS(GAME_BOOT_WAIT_MS));
#if GAME_ARRANGE_AT_BOOT
    arrange_panels();
#endif
    begin_round();

    for (;;) {
        if (s_play_mario_relay) {
            s_play_mario_relay = false;
            run_mario_relay();
        }

        if (s_dj_tap_pending) {
            uint8_t target = s_dj_tap_node;
            s_dj_tap_pending = false;
            run_dj_transport_anim(target);
        }

        if (xQueueReceive(tap_q, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_last_tap_tick = xTaskGetTickCount();
            game_shared_process_tap(&game, evt.panel_id, evt.tap_type);
        }

        GameManager snap;
        if (!game_shared_snapshot(&game, &snap)) continue;

        if (snap.is_dj_mode) {
            memset(last_panel_display, 0xFF, sizeof(last_panel_display));
            s_last_tap_tick = xTaskGetTickCount();
            prev_state = (GameState)snap.current_state;
            continue;
        }

        // --- Game Over / Disconnect Animation Hook ---
        if (snap.current_state == STATE_PAUSED_DISCONNECT) {
            if (s_started && prev_state != STATE_PAUSED_DISCONNECT) {
                printf("Game Event: Node disconnected -> Game Over animation\n");
                buzzer_play(BUZZ_WRONG);
                show_on_all(ANIM_CMD_GAMEOVER);
            }
            memset(last_panel_display, 0xFF, sizeof(last_panel_display));
            s_last_tap_tick = xTaskGetTickCount();
            prev_state = STATE_PAUSED_DISCONNECT;
            continue;
        } else if (prev_state == STATE_PAUSED_DISCONNECT) {
            show_on_all("OFF");
        }

        // --- Tap Time-Limit Check (while waiting for the 2nd card of a turn) ---
        if (s_started && snap.current_state == STATE_WAIT_SECOND_CARD) {
            if ((xTaskGetTickCount() - s_last_tap_tick) >= pdMS_TO_TICKS(TAP_TIMEOUT_MS)) {
                printf("Game Event: Tap timeout (%lu ms) -> Game Over animation\n",
                       (unsigned long)TAP_TIMEOUT_MS);
                s_started = false;
                buzzer_play(BUZZ_WRONG);
                show_on_all(ANIM_CMD_GAMEOVER);
                vTaskDelay(pdMS_TO_TICKS(2000));
                show_on_all("OFF");
                game_shared_restart(&game);
                xQueueReset(tap_q);
                memset(last_panel_display, 0xFF, sizeof(last_panel_display));
                prev_state = STATE_INIT_BOARD;
                begin_round();
                continue;
            }
        }

        // --- Panel LED State Sync ---
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

        // --- Match Evaluation & Victory Sequence ---
        if (snap.current_state == STATE_EVALUATE_MATCH) {
            uint8_t card1 = get_card_at_panel(&snap, snap.first_selection);
            uint8_t card2 = get_card_at_panel(&snap, snap.second_selection);

            if (card1 == card2) {
                printf("Game Event: Match on panels %u & %u (profile %u)\n",
                       (unsigned)(snap.first_selection + 1),
                       (unsigned)(snap.second_selection + 1),
                       (unsigned)snap.sound_profile);
                play_match_sound_for_profile(snap.sound_profile);
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                printf("Game Event: Mismatch on panels %u & %u\n",
                       (unsigned)(snap.first_selection + 1),
                       (unsigned)(snap.second_selection + 1));
                buzzer_play(BUZZ_WRONG);
                vTaskDelay(pdMS_TO_TICKS(500));
                show_on_node(snap.first_selection + 1, ANIM_CMD_GAMEOVER);
                show_on_node(snap.second_selection + 1, ANIM_CMD_GAMEOVER);
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            game_shared_evaluate(&game);
            xQueueReset(tap_q);
            s_last_tap_tick = xTaskGetTickCount();

        } else if (snap.current_state == STATE_GAME_WON) {
            printf("Game Event: Win! Playing victory animation\n");
            s_started = false;

            show_on_all(ANIM_CMD_WIN_1);
            vTaskDelay(pdMS_TO_TICKS(800));
            show_on_all(ANIM_CMD_WIN_2);
            vTaskDelay(pdMS_TO_TICKS(500));
            show_on_all(ANIM_CMD_WIN_3);
            vTaskDelay(pdMS_TO_TICKS(1000));
            show_on_all("OFF");

            vTaskDelay(pdMS_TO_TICKS(2500));
            game_shared_restart(&game);
            xQueueReset(tap_q);
            memset(last_panel_display, 0xFF, sizeof(last_panel_display));
            printf("Game restarted.\n");
#if GAME_REARRANGE_EACH_ROUND
            arrange_panels();
#endif
            begin_round();
        }

        prev_state = (GameState)snap.current_state;
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