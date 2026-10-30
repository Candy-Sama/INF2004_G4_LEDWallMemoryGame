// display.c  (Farhah's module)
#include <stdbool.h>
#include <stdio.h>

#include "display.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "node_identity.h"
#include "ledmatrix.h"
#include "imagedata.h"

#define DISPLAY_TASK_PRIORITY 2
#define DISPLAY_TASK_STACK    512

typedef struct { const uint8_t *image; uint8_t r, g, b; bool refresh; } display_cmd_t;
static QueueHandle_t display_q;
static QueueHandle_t display_q;
static volatile uint8_t s_rotation; 


void display_show_colour(uint8_t r, uint8_t g, uint8_t b) {
    display_cmd_t cmd = {NULL, r, g, b, false};
    // Never block the caller; if the panel is busy the newest command is dropped.
    xQueueSend(display_q, &cmd, 0);
}

void display_show_image(const uint8_t *rgb) {
    display_cmd_t cmd = {rgb, 0, 0, 0, false};
    xQueueSend(display_q, &cmd, 0);
}

void display_set_rotation(uint8_t quarter_turns) {
    quarter_turns &= 3;
    if (s_rotation == quarter_turns) return;
    s_rotation = quarter_turns;
    // Ask the display task to redraw whatever is currently shown.
    display_cmd_t cmd = {NULL, 0, 0, 0, true};
    xQueueSend(display_q, &cmd, 0);
}


void display_show_own_number(void) {
    static const uint8_t *const number_cards[] = {
        Number1Card_bin, Number2Card_bin, Number3Card_bin,
        Number4Card_bin, Number5Card_bin, Number6Card_bin,
    };
    int id = identity_node_id();   // 1..6 (0 = unknown)
    if (id >= 1 && id <= (int)(sizeof(number_cards) / sizeof(number_cards[0]))) {
        display_show_image(number_cards[id - 1]);
    } else {
        display_show_colour(12, 6, 0);   // should not happen: unknown boards never reach here
    }
}

static void display_task(void *arg) {
    (void)arg;
    ledmatrix_init();
    display_cmd_t cmd = {0};
    display_cmd_t incoming;
    bool have_cmd = false;
    for (;;) {
        if (xQueueReceive(display_q, &incoming, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (!incoming.refresh) {   // a refresh request just redraws the last command
                cmd = incoming;
                have_cmd = true;
            }
        }
        if (!have_cmd) {
            continue;
        }
        if (cmd.image) {
            show_image_rgb_rotated(cmd.image, s_rotation);
        } else {
            ledmatrix_fill(cmd.r, cmd.g, cmd.b);
        }
    }
}

bool display_start(void) {
    display_q = xQueueCreate(4, sizeof(display_cmd_t));
    if (!display_q) return false;
    return xTaskCreate(display_task, "display", DISPLAY_TASK_STACK, NULL,
                       DISPLAY_TASK_PRIORITY, NULL) == pdPASS;
}