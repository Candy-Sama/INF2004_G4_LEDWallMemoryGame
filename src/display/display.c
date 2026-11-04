// display.c  (Farhah's module)
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "node_identity.h"
#include "ledmatrix.h"
#include "imagedata.h"
#include "start_word.h"

#define DISPLAY_TASK_PRIORITY 4
#define DISPLAY_TASK_STACK    512
#define START_BLUE      5
#define SLOT_UNKNOWN    0xFF

typedef enum { CMD_IMAGE, CMD_COLOUR, CMD_REDRAW, CMD_START } cmd_kind_t;
typedef struct { cmd_kind_t kind; const uint8_t *image; uint8_t r, g, b; } display_cmd_t;
static QueueHandle_t display_q;
static volatile uint8_t s_rotation; 
static volatile uint8_t s_slot = SLOT_UNKNOWN;

static void send_cmd(cmd_kind_t kind, const uint8_t *image, uint8_t r, uint8_t g, uint8_t b) {
    display_cmd_t cmd = { kind, image, r, g, b };
    xQueueSend(display_q, &cmd, 0);
}

void display_show_colour(uint8_t r, uint8_t g, uint8_t b) { send_cmd(CMD_COLOUR, NULL, r, g, b); }
void display_show_image(const uint8_t *rgb)               { send_cmd(CMD_IMAGE, rgb, 0, 0, 0); }
void display_show_start_slice(void)                       { send_cmd(CMD_START, NULL, 0, 0, 0); }
void display_set_slot(uint8_t slot)                       { s_slot = slot; }

void display_set_rotation(uint8_t quarter_turns) {
    quarter_turns &= 3;
    if (s_rotation == quarter_turns) return;
    s_rotation = quarter_turns;
    send_cmd(CMD_REDRAW, NULL, 0, 0, 0);
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

static uint8_t current_slot(void) {
    uint8_t slot = s_slot;
    if (slot != SLOT_UNKNOWN) return slot;
    int id = identity_node_id();
    return (id >= 1 && id <= START_PANELS) ? (uint8_t)(id - 1) : 0;
}

static void display_task(void *arg) {
    (void)arg;
    ledmatrix_init();

    static uint8_t tile[192];
    display_cmd_t cmd = {0}, incoming;
    bool have_cmd = false, start_on = false;
    TickType_t start_end = 0;

    for (;;) {
        TickType_t wait = start_on ? pdMS_TO_TICKS(100) : pdMS_TO_TICKS(500);
        if (xQueueReceive(display_q, &incoming, wait) == pdTRUE) {
            if (incoming.kind == CMD_START) {
                start_on = true;
                start_end = xTaskGetTickCount() + pdMS_TO_TICKS(START_HOLD_MS);
            } else if (incoming.kind != CMD_REDRAW) {
                cmd = incoming;
                have_cmd = true;
            }
        }

        if (start_on && (int32_t)(xTaskGetTickCount() - start_end) >= 0) start_on = false;

        if (start_on) {
            start_word_tile(tile, current_slot(), s_rotation, START_BLUE);
            taskENTER_CRITICAL();
            show_image_rgb_rotated(tile, s_rotation);
            taskEXIT_CRITICAL();
            continue;
        }

        if (!have_cmd) continue;

        taskENTER_CRITICAL();
        if (cmd.kind == CMD_IMAGE && cmd.image) {
            show_image_rgb_rotated(cmd.image, s_rotation);
        } else {
            ledmatrix_fill(cmd.r, cmd.g, cmd.b);
        }
        taskEXIT_CRITICAL();
    }
}

bool display_start(void) {
    display_q = xQueueCreate(8, sizeof(display_cmd_t));
    if (!display_q) return false;
    return xTaskCreate(display_task, "display", DISPLAY_TASK_STACK, NULL,
                       DISPLAY_TASK_PRIORITY, NULL) == pdPASS;
}
