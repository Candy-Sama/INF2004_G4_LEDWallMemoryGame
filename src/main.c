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
#include "buzzer.h"
#include "setup.h"
#include "clock_sync.h"
#include "fatal.h"
#include "game_logic.h"
#include "board_map.h"

_Static_assert(BOARD_MAP_COUNT >= ACTIVE_PANELS,
               "BOARD_MAP must list every active panel (the join flow is only a fallback)");

#ifndef WAIT_FOR_USB
#define WAIT_FOR_USB 0
#endif

/* ------------------------------------------------------- FreeRTOS hooks */

void vApplicationMallocFailedHook(void) {
    fatal_reboot_now("FreeRTOS heap exhausted (raise configTOTAL_HEAP_SIZE)");
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    printf("stack overflow in task '%s' (raise its stack size)\n", name);
    fatal_reboot_now("stack overflow");
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
        fatal_reboot("identity failed (check BOARD_MAP)");
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

    if (!display_start()) { fatal_reboot("display failed"); }
    if (assigned && IS_GAME_MASTER && !game_start()) { fatal_reboot("game failed"); }

    if (!buzzer_start()) { fatal_reboot("buzzer failed"); }     
    if (!setup_start()) { fatal_reboot("setup failed"); }  

     if (!clock_sync_start()) { fatal_reboot("clock sync failed"); }   // before net_start()

    if (!sensor_start()) { fatal_reboot("sensor failed"); }   // before net_start()
    if (!net_start())    { fatal_reboot("net failed"); }

    if (!assigned) display_show_colour(12, 6, 0);   // dim orange until a number arrives

   

    vTaskStartScheduler();
    for (;;) {}
}