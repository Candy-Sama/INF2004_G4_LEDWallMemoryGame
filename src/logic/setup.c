#include "setup.h"
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "node_identity.h"
#include "mqtt_handler.h"
#include "buzzer.h"
#include "display.h"
#include "game_task.h"

#define SETUP_TASK_STACK      768
#define SETUP_TASK_PRIORITY   2
#define ALL_CORRECT           0x3F
#define STABLE_MS             1000    // order must stay correct this long
#define WRONG_COOLDOWN_MS     3000    // per node, avoids buzzer spam
#define SETUP_AUTOSTART_TEST  0       // 1 = start after 3 s without ultrasonic (testing only)

typedef struct { uint8_t op, arg; } setup_msg_t;

static QueueHandle_t order_q;   // net_task   -> setup_task (uint8_t mask)
static QueueHandle_t out_q;     // setup_task -> net_task   (setup_msg_t)

// Farhah provides the real one in display.c (draws this panel's slice of "START").
// This weak fallback keeps the build working until then.
__attribute__((weak)) void display_show_start_slice(void) { display_show_colour(0, 12, 0); }

static void run_action(const setup_msg_t *m) {
    if (m->op == SETUP_OP_WRONG && m->arg == identity_node_id()) {
        buzzer_play(BUZZ_WRONG);
        printf("Setup: wrong position, beep\n");
    } else if (m->op == SETUP_OP_START) {
        buzzer_play(BUZZ_JINGLE);
        display_show_start_slice();
        if (IS_GAME_MASTER) game_begin();      // node 4 only
        printf("Setup: START\n");
    }
}

static void coordinator_task(void *arg) {
    (void)arg;
    uint8_t mask = 0, last_mask = 0xFF;
    TickType_t correct_since = 0, last_beep[7] = {0};
    bool started = false;
    TickType_t boot = xTaskGetTickCount();

    while (!started) {
        uint8_t m;
        if (xQueueReceive(order_q, &m, pdMS_TO_TICKS(100)) == pdTRUE) mask = m & ALL_CORRECT;
        TickType_t now = xTaskGetTickCount();
#if SETUP_AUTOSTART_TEST
        if ((now - boot) > pdMS_TO_TICKS(3000)) mask = ALL_CORRECT;
#endif
        if (mask != last_mask) {
            last_mask = mask;
            correct_since = (mask == ALL_CORRECT) ? now : 0;
        }
        // Tell each wrong node to beep (rate limited).
        for (uint8_t n = 1; n <= 6; n++) {
            bool wrong = !(mask & (1u << (n - 1)));
            if (wrong && mask != 0 && (now - last_beep[n]) > pdMS_TO_TICKS(WRONG_COOLDOWN_MS)) {
                setup_msg_t msg = { SETUP_OP_WRONG, n };
                xQueueSend(out_q, &msg, 0);
                last_beep[n] = now;
            }
        }
        if (mask == ALL_CORRECT && correct_since &&
            (now - correct_since) > pdMS_TO_TICKS(STABLE_MS)) {
            setup_msg_t msg = { SETUP_OP_START, 0 };
            xQueueSend(out_q, &msg, 0);
            started = true;
        }
    }
    vTaskDelete(NULL);
}

bool setup_register_subscriptions(void) {
    if (!mqtt_subscribe_topic(MQTT_SETUP_TOPIC)) return false;
    if (IS_COORDINATOR && !mqtt_subscribe_topic(MQTT_ORDER_TOPIC)) return false;
    return true;
}

bool setup_handle_message(const char *topic, const uint8_t *p, size_t len) {
    if (strcmp(topic, MQTT_ORDER_TOPIC) == 0) {
        if (IS_COORDINATOR && len == 1) xQueueSend(order_q, &p[0], 0);
        return true;
    }
    if (strcmp(topic, MQTT_SETUP_TOPIC) == 0) {
        if (len == 2) { setup_msg_t m = { p[0], p[1] }; run_action(&m); }
        return true;
    }
    return false;
}

void setup_publish_output(void) {
    setup_msg_t m;
    while (xQueueReceive(out_q, &m, 0) == pdTRUE) {
        if (!mqtt_send_ex(MQTT_SETUP_TOPIC, &m, 2, 1, false, NULL, NULL))
            printf("Setup: publish failed\n");
    }
}

bool setup_start(void) {
    order_q = xQueueCreate(4, sizeof(uint8_t));
    out_q   = xQueueCreate(8, sizeof(setup_msg_t));
    if (!order_q || !out_q) return false;
    if (!IS_COORDINATOR) return true;
    return xTaskCreate(coordinator_task, "setup", SETUP_TASK_STACK, NULL,
                       SETUP_TASK_PRIORITY, NULL) == pdPASS;
}