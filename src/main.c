#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/rand.h"
#include "FreeRTOS.h"
#include "task.h"
#include "grid_config.h"
#include "node_identity.h"
#include "display.h"
#include "sensor.h"
#include "net.h"
#include "game_task.h"

#ifndef WAIT_FOR_USB
#define WAIT_FOR_USB 0
#endif

/* ------------------------------------------------- unknown-board helper */

// A board missing from config/board_map.h repeats its ID so it can be copied
// even if the serial monitor was opened late.
static void unknown_board_task(void *arg) {
    (void)arg;
    for (;;) {
        printf("UNKNOWN BOARD: %s  (add it to config/board_map.h)\n", identity_board_id());
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ------------------------------------------------------- FreeRTOS hooks */

void vApplicationMallocFailedHook(void) {
    printf("FATAL: FreeRTOS heap exhausted (raise configTOTAL_HEAP_SIZE).\n");
    for (;;) {}
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    printf("FATAL: stack overflow in task '%s' (raise its stack size).\n", name);
    for (;;) {}
}

/* ------------------------------------------------------------------ main */

int main(void) {
    stdio_init_all();
#if WAIT_FOR_USB
    // Open the USB serial monitor to start this teaching/demo firmware.
    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    sleep_ms(1000);
#endif

    bool known = identity_init();
    if (known) {
        printf("\n--- PANEL %d (row %d, col %d) board %s ---\n",
               identity_node_id(), grid_row(identity_node_id()),
               grid_col(identity_node_id()), identity_board_id());
    } else {
        printf("\n--- UNKNOWN BOARD %s ---\n", identity_board_id());
    }

    srand(get_rand_32());

    if (!display_start()) { printf("display failed\n"); for (;;) {} }
    if (known && IS_GAME_MASTER && !game_start()) { printf("game failed\n"); for (;;) {} }

    if (known) {
        if (!sensor_start()) { printf("sensor failed\n"); for (;;) {} }   // before net_start()
        if (!net_start())    { printf("net failed\n");    for (;;) {} }
    } else {
        display_show_colour(12, 6, 0);   // dim orange = board not in BOARD_MAP
        xTaskCreate(unknown_board_task, "unknown", 512, NULL, 1, NULL);
    }

    vTaskStartScheduler();
    for (;;) {}
}