// buzzer.h
#pragma once
#include <stdbool.h>

typedef enum {
    BUZZ_NONE = 0,          // 0 is never played
    BUZZ_JINGLE,            // START jingle (setup.c)
    BUZZ_WRONG,             // wrong card / wrong position
    BUZZ_MATCH_STANDARD,    // sound profile 0
    BUZZ_MATCH_MARIO,       // sound profile 1
    BUZZ_MATCH_PIANO,       // sound profile 2
    BUZZ_DJ_1,
    BUZZ_DJ_2,
    BUZZ_DJ_3,
    BUZZ_DJ_4,
    BUZZ_DJ_5,
    BUZZ_DJ_6
} buzz_id_t;

bool buzzer_start(void);
void buzzer_play(buzz_id_t id);    // non-blocking, safe from any task