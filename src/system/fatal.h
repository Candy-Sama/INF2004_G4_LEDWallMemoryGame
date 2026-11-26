#pragma once

// Unrecoverable error: show a red panel (if the scheduler is running), wait a few
// seconds so it can be seen and read on USB serial, then reboot through the watchdog.
// Use instead of vTaskDelete(NULL) or for(;;){} so a dead panel recovers by itself.
void fatal_reboot(const char *why) __attribute__((noreturn));

// Same, but immediate and with no display/RTOS calls. Safe inside FreeRTOS hooks.
void fatal_reboot_now(const char *why) __attribute__((noreturn));