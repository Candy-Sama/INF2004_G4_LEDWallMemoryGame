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

    if (!identity_init()) {                      // fails only if BOARD_MAP itself is invalid
        printf("identity failed (check BOARD_MAP)\n");
        for (;;) {}
    }

    bool assigned = identity_is_assigned();
    if (assigned) {
        printf("\n--- PANEL %d (row %d, col %d) board %s ---\n",
               identity_node_id(), grid_row(identity_node_id()),
               grid_col(identity_node_id()), identity_board_id());
    } else {
        printf("\n--- UNASSIGNED BOARD %s: will request a number from the master ---\n",
               identity_board_id());
    }

    srand(get_rand_32());

    if (!display_start()) { printf("display failed\n"); for (;;) {} }
    if (assigned && IS_GAME_MASTER && !game_start()) { printf("game failed\n"); for (;;) {} }

    if (!sensor_start()) { printf("sensor failed\n"); for (;;) {} }   // before net_start()
    if (!net_start())    { printf("net failed\n");    for (;;) {} }

    if (!assigned) display_show_colour(12, 6, 0);   // dim orange until a number arrives

    vTaskStartScheduler();
    for (;;) {}
}