#ifndef FAULTS_H
#define FAULTS_H

#include <stdint.h>
#include "wire_format.h"   /* FAULT_* bit definitions */

/*
 * One shared fault bitmask (a single byte) instead of separate variables.
 *
 * Several tasks set bits (sensor task: FAULT_IMU; network task: Wi-Fi/MQTT/publish
 * faults; display: FAULT_DISPLAY), so every read-modify-write is wrapped in a
 * FreeRTOS critical section. The RP2040 (Cortex-M0+) has no exclusive load/store,
 * so C11 atomics would not be lock-free. Do not call from an ISR.
 *
 * Bits are sticky until fault_clear() is called.
 */
void    fault_set(uint8_t mask);
void    fault_clear(uint8_t mask);
uint8_t fault_get(void);   /* a one-byte read is atomic on its own */

#endif
