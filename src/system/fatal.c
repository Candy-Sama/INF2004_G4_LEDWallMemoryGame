#include "fatal.h"
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "FreeRTOS.h"
#include "task.h"
#include "display.h"

#define FATAL_SHOW_MS    3000
#define FATAL_REBOOT_MS  10

void fatal_reboot_now(const char *why) {
    printf("FATAL: %s - rebooting\n", why);
    watchdog_reboot(0, 0, FATAL_REBOOT_MS);
    for (;;) { tight_loop_contents(); }
}

void fatal_reboot(const char *why) {
    printf("FATAL: %s - rebooting in %d ms\n", why, FATAL_SHOW_MS);
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        display_show_colour(20, 0, 0);               // red = this panel hit a fatal error
        vTaskDelay(pdMS_TO_TICKS(FATAL_SHOW_MS));
    } else {
        sleep_ms(FATAL_SHOW_MS);                     // display task is not running yet
    }
    fatal_reboot_now(why);
}