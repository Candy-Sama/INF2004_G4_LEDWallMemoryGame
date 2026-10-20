#ifndef SERIAL_TEST_STDLIB_H
#define SERIAL_TEST_STDLIB_H
#include <stdbool.h>
#include <stdint.h>
#define PICO_ERROR_TIMEOUT -1
bool stdio_usb_connected(void);
int getchar_timeout_us(uint32_t timeout);
void sleep_ms(uint32_t duration);
#endif
