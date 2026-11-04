#include "setup.h"
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "net.h"
#include "buzzer.h"
#include "display.h"
#include "game_task.h"

#define SETUP_TASK_STACK      768
#define SETUP_TASK_PRIORITY   2
#define ALL_CORRECT           0x3F
#define STABLE_MS             1000
#define WRONG_COOLDOWN_MS     3000
#define SETUP_AUTOSTART_TEST  0   // 1 = start after 3 s without ultrasonic (testing only)

typedef struct { uint8_t op, arg; } setup_msg_t;

static QueueHandle_t order_q;

// // Farhah provides the real one in display.c. This fallback keeps the build working.
// __attribute__((weak)) void display_show_start_slice(void) { display_show_colour(0, 12, 0); }

static void run_action(const setup_msg_t *m) {
    if (m->op == SETUP_OP_WRONG && m->arg == identity_node_id()) {
        buzzer_play(BUZZ_WRONG);
        printf("Setup: wrong position, beep\n");
    } else if (m->op == SETUP_OP_ARRANGE) {
        display_set_slot(0xFF);
        display_show_colour(0, 0, 5);
        printf("Setup: arrange, tap the panels in order\n");
    } else if (m->op == SETUP_OP_SLOT) {
        if ((m->arg & 0x0F) == identity_node_id()) {
            display_set_slot(m->arg >> 4);
            display_show_colour(0, 5, 0);
            printf("Setup: slot %d\n", m->arg >> 4);
        }
    } else if (m->op == SETUP_OP_START) {
        buzzer_play(BUZZ_JINGLE);
        display_show_start_slice();
        printf("Setup: START\n");
    }
}

// ---- net handlers (inside net_task: non-blocking only) ----

static void on_setup(const char *topic, const uint8_t *p, size_t len) {
    (void)topic;
    if (len != 2) return;
    setup_msg_t m = { p[0], p[1] };
    run_action(&m);
}

static void on_order(const char *topic, const uint8_t *p, size_t len) {
    (void)topic;
    if (len == 1) xQueueSend(order_q, &p[0], 0);
}

// ---- coordinator (node 1) ----

static void coordinator_task(void *arg) {
    (void)arg;
    uint8_t mask = 0, last_mask = 0xFF;
    TickType_t correct_since = 0, last_beep[7] = {0};
    TickType_t boot = xTaskGetTickCount();
    bool started = false;

    while (!started) {
        uint8_t m;
        if (xQueueReceive(order_q, &m, pdMS_TO_TICKS(100)) == pdTRUE) mask = m & ALL_CORRECT;
        TickType_t now = xTaskGetTickCount();
#if SETUP_AUTOSTART_TEST
        if ((now - boot) > pdMS_TO_TICKS(3000)) mask = ALL_CORRECT;
#else
        (void)boot;
#endif
        if (mask != last_mask) {
            last_mask = mask;
            correct_since = (mask == ALL_CORRECT) ? now : 0;
        }
        for (uint8_t n = 1; n <= 6; n++) {
            bool wrong = !(mask & (1u << (n - 1)));
            if (wrong && mask != 0 && (now - last_beep[n]) > pdMS_TO_TICKS(WRONG_COOLDOWN_MS)) {
                setup_msg_t msg = { SETUP_OP_WRONG, n };
                net_publish(MQTT_SETUP_TOPIC, &msg, sizeof(msg), false);
                last_beep[n] = now;
            }
        }
        if (mask == ALL_CORRECT && correct_since &&
            (now - correct_since) > pdMS_TO_TICKS(STABLE_MS)) {
            setup_msg_t msg = { SETUP_OP_START, 0 };
            net_publish(MQTT_SETUP_TOPIC, &msg, sizeof(msg), false);
            started = true;
        }
    }
    vTaskDelete(NULL);
}

bool setup_start(void) {
    order_q = xQueueCreate(4, sizeof(uint8_t));
    if (!order_q) return false;
    if (!net_register_handler(MQTT_SETUP_TOPIC, on_setup)) return false;
    if (!IS_COORDINATOR) return true;
    if (!net_register_handler(MQTT_ORDER_TOPIC, on_order)) return false;
    return xTaskCreate(coordinator_task, "setup", SETUP_TASK_STACK, NULL,
                       SETUP_TASK_PRIORITY, NULL) == pdPASS;
}