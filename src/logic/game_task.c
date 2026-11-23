#include "game_task.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "game_logic.h"
#include "net.h"
#include "node_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "buzzer.h"
#include "setup.h"
#include "start_word.h"

#define GAME_TASK_PRIORITY          3
#define GAME_TASK_STACK             1024
#define TAP_QUEUE_LEN               16
#define STATUS_QUEUE_LEN            16
#define GAME_TEST_FORCE_ONLINE      0
#define GAME_TEST_AUTOSTART         0
#define GAME_ARRANGE_AT_BOOT        1
#define GAME_REARRANGE_EACH_ROUND   0
#define BOOT_ONLINE_TIMEOUT_MS      10000
#define TAP_TIMEOUT_MS              20000
#define MARIO_VIBRATION_GUARD_MS    600

#define ANIM_CMD_WIN_1              "win"
#define ANIM_CMD_WIN_2              "OFF"
#define ANIM_CMD_WIN_3              "win"
#define ANIM_CMD_GAMEOVER           "lose"

typedef struct {
    uint8_t node;
    bool    online;
} status_evt_t;

static GameShared       game;
static QueueHandle_t    tap_q;
static QueueHandle_t    status_q;
static bool             s_started;
static bool             s_use_fart_kit     = true;
static bool             s_mario_active     = false;
static TickType_t       s_mario_end_tick   = 0;
static uint8_t          s_prev_dj_node     = 0;
static TickType_t       s_last_tap_tick    = 0;
static uint32_t         s_last_single_ms   = 0;
static uint8_t          s_last_single_node = 0;

static uint8_t s_node_to_slot[NUM_PANELS] = {0, 1, 2, 3, 4, 5};
static uint8_t s_slot_to_node[NUM_PANELS] = {1, 2, 3, 4, 5, 6};

static int8_t node_to_slot(uint8_t node) {
    if (node < 1 || node > NUM_PANELS) return -1;
    uint8_t slot = s_node_to_slot[node - 1];
    return (slot < ACTIVE_PANELS) ? (int8_t)slot : -1;
}

static uint8_t slot_to_node(uint8_t slot) {
    if (slot >= ACTIVE_PANELS) return 0;
    uint8_t node = s_slot_to_node[slot];
    return (node >= 1 && node <= NUM_PANELS) ? node : 0;
}

void game_begin(void) {
    s_last_tap_tick = xTaskGetTickCount();
    s_started = true;
    printf("Game: begin\n");
}

// ---- display & sound helpers ----

static void show_on_node(uint8_t n, const char *payload) {
    if (n < 1 || n > NUM_PANELS || !payload) return;
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/node%u/command", MQTT_TOPIC_ROOT, (unsigned)n);
    net_publish(topic, payload, strlen(payload), false);
}

static void show_on_active_slots(const char *payload) {
    for (uint8_t slot = 0; slot < ACTIVE_PANELS; slot++) {
        uint8_t node = slot_to_node(slot);
        if (node != 0) {
            show_on_node(node, payload);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static void play_match_sound_for_profile(uint8_t profile) {
    if (profile == 0)      buzzer_play(BUZZ_MATCH_STANDARD);
    else if (profile == 1) buzzer_play(BUZZ_MATCH_MARIO);
    else if (profile == 2) buzzer_play(BUZZ_MATCH_PIANO);
}

// ---- non-blocking Mario relay control ----

static void stop_mario_relay(void) {
    if (!s_mario_active) return;
    s_mario_active = false;
    show_on_active_slots("stop");
    printf("Mario Relay: stopped remote audio\n");
}

static void start_mario_relay(uint8_t trigger_node) {
    stop_mario_relay();

    uint8_t mask = game_shared_get_online_mask(&game);
    uint8_t connected[ACTIVE_PANELS];
    uint8_t count = 0;

    for (uint8_t slot = 0; slot < ACTIVE_PANELS; slot++) {
        uint8_t node = slot_to_node(slot);
        if (node != 0 && (mask & (1u << (node - 1)))) {
            connected[count++] = node;
        }
    }
    if (count == 0) {
        connected[0] = trigger_node;
        count = 1;
    }

    uint8_t  total_notes = buzzer_mario_note_count();
    uint32_t total_ms    = buzzer_mario_slice_ms(0, total_notes);
    uint8_t  note_idx    = 0;
    uint32_t elapsed_ms  = 0;

    printf("Mario Relay: upfront schedule (%lu ms) across %u online slot(s)\n",
           (unsigned long)total_ms, (unsigned)count);

    // 1. Trigger wide-screen animation on all connected panels
    for (uint8_t k = 0; k < count; k++) {
        char anim_cmd[32];
        snprintf(anim_cmd, sizeof(anim_cmd), "mario_all:%u:%u:%lu",
                 (unsigned)k, (unsigned)count, (unsigned long)total_ms);
        show_on_node(connected[k], anim_cmd);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 2. Schedule all audio slices upfront with exact start offsets (immune to Wi-Fi jitter)
    for (uint8_t k = 0; k < count && note_idx < total_notes; k++) {
        uint32_t target_ms      = ((uint32_t)(k + 1) * total_ms) / count;
        uint8_t  start          = note_idx;
        uint32_t slice_delay_ms = elapsed_ms;

        if (k == count - 1) {
            note_idx = total_notes;
        } else {
            while (note_idx < total_notes && elapsed_ms < target_ms) {
                elapsed_ms += buzzer_mario_slice_ms(note_idx, (uint8_t)(note_idx + 1));
                note_idx++;
            }
        }

        char audio_cmd[24];
        snprintf(audio_cmd, sizeof(audio_cmd), "ms:%u:%u:%lu",
                 (unsigned)start, (unsigned)note_idx, (unsigned long)slice_delay_ms);
        show_on_node(connected[k], audio_cmd);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_mario_active   = true;
    s_mario_end_tick = xTaskGetTickCount() + pdMS_TO_TICKS(total_ms);
}

// ---- mode switching & DJ/Fart animations ----

static void exit_dj_to_memory(uint8_t last_panel_display[NUM_PANELS]) {
    stop_mario_relay();
    s_prev_dj_node = 0;

    GameManager snap;
    if (game_shared_snapshot(&game, &snap) && snap.is_dj_mode) {
        game_shared_toggle_dj_mode(&game);
    }
    game_shared_clear_selection(&game);
    memset(last_panel_display, 0xFF, NUM_PANELS);
    s_last_tap_tick = xTaskGetTickCount();
    printf("Switched modes! Memory Game mode is now ON\n");
    show_on_active_slots("OFF");
}

static void handle_node1_triple_tap(uint8_t last_panel_display[NUM_PANELS]) {
    stop_mario_relay();
    s_prev_dj_node = 0;

    GameManager snap;
    if (!game_shared_snapshot(&game, &snap)) return;

    if (!snap.is_dj_mode) {
        s_use_fart_kit = true;
        game_shared_toggle_dj_mode(&game);
        printf("Switched modes! FART mode is now ON\n");
        show_on_active_slots("spade");
    } else if (s_use_fart_kit) {
        s_use_fart_kit = false;
        printf("Switched modes! DJ DRUM mode is now ON\n");
        show_on_active_slots("Clover");
    } else {
        exit_dj_to_memory(last_panel_display);
    }
}

static void run_dj_transport_anim(uint8_t target_node) {
    int8_t to_slot = node_to_slot(target_node);
    if (to_slot < 0) return;

    char cmd[24];
    uint8_t kit = s_use_fart_kit ? 1 : 0;

    if (s_prev_dj_node == 0 || s_prev_dj_node == target_node) {
        snprintf(cmd, sizeof(cmd), "burst:%u:%u", (unsigned)target_node, (unsigned)kit);
        show_on_node(target_node, cmd);
        s_prev_dj_node = target_node;
        return;
    }

    int8_t from_slot = node_to_slot(s_prev_dj_node);
    if (from_slot < 0) {
        s_prev_dj_node = target_node;
        return;
    }

    uint8_t from_node = s_prev_dj_node;
    int8_t  step      = (to_slot > from_slot) ? 1 : -1;
    char    dir       = (step > 0) ? 'R' : 'L';

    snprintf(cmd, sizeof(cmd), "tx_out:%u:%u:%c", (unsigned)from_node, (unsigned)kit, dir);
    show_on_node(from_node, cmd);
    vTaskDelay(pdMS_TO_TICKS(110));

    for (int8_t s = from_slot + step; s != to_slot; s += step) {
        GameManager snap;
        if (!game_shared_snapshot(&game, &snap) || !snap.is_dj_mode) return;

        uint8_t mid_node = slot_to_node((uint8_t)s);
        if (mid_node != 0) {
            snprintf(cmd, sizeof(cmd), "tx_pass:%u:%u:%c", (unsigned)from_node, (unsigned)kit, dir);
            show_on_node(mid_node, cmd);
            vTaskDelay(pdMS_TO_TICKS(110));
        }
    }

    snprintf(cmd, sizeof(cmd), "tx_in:%u:%u:%u:%c",
             (unsigned)from_node, (unsigned)target_node, (unsigned)kit, dir);
    show_on_node(target_node, cmd);
    s_prev_dj_node = target_node;
}

static void handle_dj_tap(const tap_evt_t *evt) {
    uint8_t node = evt->from_node;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (evt->tap_type == 1) {
        static const char *dj_cmds[NUM_PANELS]   = { "dj1", "dj2", "dj3", "dj4", "dj5", "dj6" };
        static const char *fart_cmds[NUM_PANELS] = { "fart1", "fart2", "fart3", "fart4", "fart5", "fart6" };

        stop_mario_relay();
        s_last_single_ms   = now_ms;
        s_last_single_node = node;
        show_on_node(node, s_use_fart_kit ? fart_cmds[node - 1] : dj_cmds[node - 1]);
        run_dj_transport_anim(node);
    } else if (evt->tap_type == 2) {
        if (s_last_single_node != 0 && s_last_single_node != node &&
            (now_ms - s_last_single_ms) < MARIO_VIBRATION_GUARD_MS) {
            printf("Mario Relay: ignored table-vibration double tap on node %u\n", (unsigned)node);
            return;
        }
        start_mario_relay(node);
    } else if (node == 2 && evt->tap_type == 3) {
        stop_mario_relay();
        s_use_fart_kit = !s_use_fart_kit;
        s_prev_dj_node = 0;
        printf("DJ Sound Kit: %s\n", s_use_fart_kit ? "FART" : "DRUMS");
        show_on_active_slots(s_use_fart_kit ? "spade" : "Clover");
    }
}

// ---- net handlers (run inside net_task: strictly non-blocking, zero mutexes) ----

static void on_tap(const char *topic, const uint8_t *p, size_t len) {
    (void)topic;
    if (len != 1) return;

    uint8_t node     = p[0] & 0x0F;
    uint8_t tap_type = (p[0] >> 6) & 0x03;

    if (node < 1 || node > NUM_PANELS) return;
    if (tap_type < 1 || tap_type > 3) return;

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
    status_evt_t evt = {
        .node   = (uint8_t)node,
        .online = (len == n && memcmp(p, MQTT_STATUS_MESSAGE, n) == 0)
    };
    xQueueSend(status_q, &evt, 0);
}

static void drain_status_queue(void) {
    static TickType_t last_print[NUM_PANELS];
    static bool printed_once[NUM_PANELS];
    static bool last_online[NUM_PANELS];

    status_evt_t evt;
    while (xQueueReceive(status_q, &evt, 0) == pdTRUE) {
        uint8_t idx = (uint8_t)(evt.node - 1);
        TickType_t now = xTaskGetTickCount();

        if (!printed_once[idx] || last_online[idx] != evt.online ||
            (now - last_print[idx]) >= pdMS_TO_TICKS(30000)) {
            printf("Presence: node %u %s\n", (unsigned)evt.node, evt.online ? "online" : "offline");
            last_print[idx]   = now;
            last_online[idx]  = evt.online;
            printed_once[idx] = true;
        }
        game_shared_set_online(&game, idx, evt.online);
    }
}

// ---- setup & main task ----

static void send_setup(uint8_t op, uint8_t arg) {
    uint8_t msg[2] = { op, arg };
    net_publish(MQTT_SETUP_TOPIC, msg, sizeof(msg), false);
}

static uint8_t wait_for_panels_online(void) {
    TickType_t start = xTaskGetTickCount();
    drain_status_queue();
    uint8_t online = game_shared_online_count(&game);

    while (online < ACTIVE_PANELS) {
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(BOOT_ONLINE_TIMEOUT_MS)) {
            if (online > 0) {
                printf("Boot: timeout waiting for %u panels; continuing with %u online\n",
                       (unsigned)ACTIVE_PANELS, (unsigned)online);
                break;
            }
            printf("Boot: still 0/%u panels online - check MQTT broker IP & Mosquitto!\n",
                   (unsigned)ACTIVE_PANELS);
            start = xTaskGetTickCount();
        }
        printf("Boot: waiting for %u panels online (%u/%u ready)...\n",
               (unsigned)ACTIVE_PANELS, (unsigned)online, (unsigned)ACTIVE_PANELS);
        vTaskDelay(pdMS_TO_TICKS(500));
        drain_status_queue();
        online = game_shared_online_count(&game);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    drain_status_queue();
    return game_shared_online_count(&game);
}

static void arrange_panels(uint8_t online_count) {
    uint8_t target = (online_count > 0 && online_count < ACTIVE_PANELS) ? online_count : ACTIVE_PANELS;
    uint8_t placed_mask = 0;
    uint8_t count = 0;

    xQueueReset(tap_q);
    send_setup(SETUP_OP_ARRANGE, 0);
    printf("Arrange: tap %u online panel(s) in your desired sequence\n", (unsigned)target);

    TickType_t last_prompt = xTaskGetTickCount();

    while (count < target) {
        drain_status_queue();

        if (count == 0 && (xTaskGetTickCount() - last_prompt) >= pdMS_TO_TICKS(2500)) {
            send_setup(SETUP_OP_ARRANGE, 0);
            last_prompt = xTaskGetTickCount();
        }

        tap_evt_t evt;
        if (xQueueReceive(tap_q, &evt, pdMS_TO_TICKS(250)) != pdTRUE) continue;
        if (evt.tap_type != 1) continue;

        uint8_t node = evt.from_node;
        if (node < 1 || node > NUM_PANELS) continue;
        if (placed_mask & (1u << (node - 1))) continue;

        placed_mask |= (uint8_t)(1u << (node - 1));
        s_node_to_slot[node - 1] = count;
        s_slot_to_node[count]    = node;

        send_setup(SETUP_OP_SLOT, (uint8_t)((count << 4) | node));
        printf("Arrange: slot %u <- node %u\n", (unsigned)count, (unsigned)node);
        count++;
    }

    // Backfill remaining slots with any unplaced nodes so late joiners map cleanly
    for (uint8_t n = 1; n <= NUM_PANELS && count < NUM_PANELS; n++) {
        if (!(placed_mask & (1u << (n - 1)))) {
            s_node_to_slot[n - 1] = count;
            s_slot_to_node[count] = n;
            count++;
        }
    }

    printf("Arrange complete: sequence = [");
    for (uint8_t i = 0; i < ACTIVE_PANELS; i++) {
        printf("%u%s", (unsigned)s_slot_to_node[i], (i + 1 < ACTIVE_PANELS) ? " -> " : "");
    }
    printf("]\n");
    vTaskDelay(pdMS_TO_TICKS(800));
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

    uint8_t online_at_boot = wait_for_panels_online();
#if GAME_ARRANGE_AT_BOOT
    arrange_panels(online_at_boot);
#else
    (void)online_at_boot;
#endif
    begin_round();

    for (;;) {
        drain_status_queue();

        // Check if upfront-scheduled Mario relay finished naturally
        if (s_mario_active && (int32_t)(xTaskGetTickCount() - s_mario_end_tick) >= 0) {
            s_mario_active = false;
            s_prev_dj_node = 0;
            s_last_tap_tick = xTaskGetTickCount();
            GameManager end_snap;
            if (game_shared_snapshot(&game, &end_snap) && end_snap.is_dj_mode) {
                show_on_active_slots(s_use_fart_kit ? "spade" : "Clover");
            }
        }

        if (xQueueReceive(tap_q, &evt, pdMS_TO_TICKS(40)) == pdTRUE) {
            if (evt.from_node == 1 && evt.tap_type == 3) {
                handle_node1_triple_tap(last_panel_display);
            } else {
                GameManager mode_snap;
                if (game_shared_snapshot(&game, &mode_snap) && mode_snap.is_dj_mode) {
                    handle_dj_tap(&evt);
                } else if (evt.from_node >= 2 && evt.tap_type == 3) {
                    uint8_t new_prof = game_shared_cycle_sound_profile(&game);
                    play_match_sound_for_profile(new_prof);
                    s_last_tap_tick = xTaskGetTickCount();
                } else if (s_started && evt.tap_type == 1) {
                    int8_t slot = node_to_slot(evt.from_node);
                    if (slot >= 0) {
                        s_last_tap_tick = xTaskGetTickCount();
                        game_shared_process_tap(&game, (uint8_t)slot, evt.tap_type);
                    }
                }
            }
        }

        GameManager snap;
        if (!game_shared_snapshot(&game, &snap)) continue;

        if (snap.is_dj_mode) {
            memset(last_panel_display, 0xFF, sizeof(last_panel_display));
            s_last_tap_tick = xTaskGetTickCount();
            prev_state = (GameState)snap.current_state;
            continue;
        }

        // --- Disconnect Pause Hook ---
        if (snap.current_state == STATE_PAUSED_DISCONNECT) {
            if (s_started && prev_state != STATE_PAUSED_DISCONNECT) {
                printf("Game Event: Node disconnected -> Pausing round\n");
                buzzer_play(BUZZ_WRONG);
                show_on_active_slots(ANIM_CMD_GAMEOVER);
            }
            memset(last_panel_display, 0xFF, sizeof(last_panel_display));
            s_last_tap_tick = xTaskGetTickCount();
            prev_state = STATE_PAUSED_DISCONNECT;
            continue;
        } else if (prev_state == STATE_PAUSED_DISCONNECT) {
            memset(last_panel_display, 0xFF, sizeof(last_panel_display));
            show_on_active_slots("OFF");
        }

        // --- Tap Time-Limit Check ---
        if (s_started && snap.current_state == STATE_WAIT_SECOND_CARD) {
            if ((xTaskGetTickCount() - s_last_tap_tick) >= pdMS_TO_TICKS(TAP_TIMEOUT_MS)) {
                printf("Game Event: Tap timeout (%lu ms)\n", (unsigned long)TAP_TIMEOUT_MS);
                s_started = false;
                buzzer_play(BUZZ_WRONG);
                show_on_active_slots(ANIM_CMD_GAMEOVER);
                vTaskDelay(pdMS_TO_TICKS(1800));
                show_on_active_slots("OFF");
                game_shared_restart(&game);
                xQueueReset(tap_q);
                memset(last_panel_display, 0xFF, sizeof(last_panel_display));
                prev_state = STATE_INIT_BOARD;
                begin_round();
                continue;
            }
        }

        // --- Panel LED State Sync ---
        for (uint8_t slot = 0; slot < ACTIVE_PANELS; slot++) {
            uint8_t phys_node = slot_to_node(slot);
            if (phys_node == 0) continue;

            bool is_revealed = (snap.matched_bitmask & (1u << slot)) ||
                               (snap.first_selection == slot) ||
                               (snap.second_selection == slot);

            uint8_t desired_display = is_revealed ? get_card_at_panel(&snap, slot) : 0xFF;

            if (desired_display != last_panel_display[slot]) {
                last_panel_display[slot] = desired_display;

                const char *payload = "OFF";
                if (desired_display == 1)      payload = "diamond";
                else if (desired_display == 2) payload = "Clover";
                else if (desired_display == 3) payload = "spade";

                show_on_node(phys_node, payload);
            }
        }

        // --- Match Evaluation & Per-Match Star Feedback ---
        if (snap.current_state == STATE_EVALUATE_MATCH) {
            uint8_t slot1 = snap.first_selection;
            uint8_t slot2 = snap.second_selection;
            uint8_t node1 = slot_to_node(slot1);
            uint8_t node2 = slot_to_node(slot2);
            uint8_t card1 = get_card_at_panel(&snap, slot1);
            uint8_t card2 = get_card_at_panel(&snap, slot2);

            if (card1 == card2 && card1 != 0) {
                printf("Game Event: Match on slots %u & %u (nodes %u & %u)\n",
                       (unsigned)(slot1 + 1), (unsigned)(slot2 + 1),
                       (unsigned)node1, (unsigned)node2);
                vTaskDelay(pdMS_TO_TICKS(500));
                play_match_sound_for_profile(snap.sound_profile);
                show_on_node(node1, ANIM_CMD_WIN_1);
                show_on_node(node2, ANIM_CMD_WIN_1);
                vTaskDelay(pdMS_TO_TICKS(900));

                last_panel_display[slot1] = 0xFF;
                last_panel_display[slot2] = 0xFF;
            } else {
                printf("Game Event: Mismatch on slots %u & %u (nodes %u & %u)\n",
                       (unsigned)(slot1 + 1), (unsigned)(slot2 + 1),
                       (unsigned)node1, (unsigned)node2);
                buzzer_play(BUZZ_WRONG);
                vTaskDelay(pdMS_TO_TICKS(450));
                show_on_node(node1, ANIM_CMD_GAMEOVER);
                show_on_node(node2, ANIM_CMD_GAMEOVER);
                vTaskDelay(pdMS_TO_TICKS(550));
            }

            game_shared_evaluate(&game);
            xQueueReset(tap_q);
            s_last_tap_tick = xTaskGetTickCount();

        } else if (snap.current_state == STATE_GAME_WON) {
            printf("Game Event: All pairs matched! Playing full victory stars\n");
            s_started = false;

            show_on_active_slots(ANIM_CMD_WIN_1);
            vTaskDelay(pdMS_TO_TICKS(800));
            show_on_active_slots(ANIM_CMD_WIN_2);
            vTaskDelay(pdMS_TO_TICKS(400));
            show_on_active_slots(ANIM_CMD_WIN_3);
            vTaskDelay(pdMS_TO_TICKS(1000));
            show_on_active_slots("OFF");

            vTaskDelay(pdMS_TO_TICKS(2000));
            game_shared_restart(&game);
            xQueueReset(tap_q);
            memset(last_panel_display, 0xFF, sizeof(last_panel_display));
            printf("Game restarted.\n");
#if GAME_REARRANGE_EACH_ROUND
            arrange_panels(game_shared_online_count(&game));
#endif
            begin_round();
        }

        prev_state = (GameState)snap.current_state;
    }
}

bool game_start(void) {
    tap_q = xQueueCreate(TAP_QUEUE_LEN, sizeof(tap_evt_t));
    status_q = xQueueCreate(STATUS_QUEUE_LEN, sizeof(status_evt_t));
    if (!tap_q || !status_q || !game_shared_init(&game)) return false;
    s_started = GAME_TEST_AUTOSTART;
    printf("sizeof(GameManager)=%u, ACTIVE_PANELS=%u\n",
           (unsigned)sizeof(GameManager), (unsigned)ACTIVE_PANELS);
#if GAME_TEST_FORCE_ONLINE
    for (uint8_t n = 0; n < ACTIVE_PANELS; n++) game_shared_set_online(&game, n, true);
#endif
    if (!net_register_handler(MQTT_TAP_TOPIC, on_tap)) return false;
    if (!net_register_handler(MQTT_TOPIC_ROOT "/+/status", on_status)) return false;
    return xTaskCreate(game_task, "game", GAME_TASK_STACK, NULL,
                       GAME_TASK_PRIORITY, NULL) == pdPASS;
}