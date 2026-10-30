// buzzer.h
#pragma once
#include <stdbool.h>
typedef enum { BUZZ_JINGLE = 1, BUZZ_WRONG } buzz_id_t;
bool buzzer_start(void);
void buzzer_play(buzz_id_t id);    // non-blocking, safe from any task