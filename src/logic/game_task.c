#include "game_task.h"
#include <stdio.h>
#include "game_logic.h"
#include "mqtt_handler.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#define GAME_TASK_PRIORITY 3
#define GAME_TASK_STACK    1024
#define GAME_TEST_FORCE_ONLINE 1

typedef struct { uint8_t bytes[4]; uint8_t len; uint8_t kind; } net_out_t;
enum { OUT_STATE, OUT_REVEAL };

static GameShared    game;
static QueueHandle_t tap_q;
static QueueHandle_t net_out_q;

static void game_task(void *arg) {
    (void)arg;
    tap_evt_t evt;
    for (;;) {
        if (xQueueReceive(tap_q, &evt, pdMS_TO_TICKS(250)) == pdTRUE) {
            game_shared_process_input(&game, evt.panel_id);
        }

        GameManager snap;
        if (!game_shared_snapshot(&game, &snap)) continue;

        net_out_t out = { .kind = OUT_STATE, .len = 2 };
        game_pack_state(&snap, out.bytes);
        xQueueSend(net_out_q, &out, 0);

        if (snap.current_state == STATE_EVALUATE_MATCH) {
            net_out_t rev = { .kind = OUT_REVEAL, .len = 1 };
            rev.bytes[0] = (snap.first_selection & 7) | ((snap.second_selection & 7) << 3);
            xQueueSend(net_out_q, &rev, 0);

            vTaskDelay(pdMS_TO_TICKS(1000));      // lock NOT held
            game_shared_evaluate(&game);
            xQueueReset(tap_q);
        }
    }
}

void game_handle_tap_payload(const uint8_t *p, size_t len) {
    if (len != 1) return;
    tap_evt_t evt = { .panel_id = p[0] & 7, .from_node = (p[0] >> 3) & 7 };
    xQueueSend(tap_q, &evt, 0);
}

void game_publish_output(void) {
    net_out_t out;
    while (xQueueReceive(net_out_q, &out, 0) == pdTRUE) {
        const char *topic = (out.kind == OUT_STATE) ? MQTT_STATE_TOPIC : MQTT_REVEAL_TOPIC;
        mqtt_send_ex(topic, (const char *)out.bytes, out.len, 1,
                     out.kind == OUT_STATE, NULL, NULL);
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