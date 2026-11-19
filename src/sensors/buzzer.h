// buzzer.h
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BUZZ_NONE = 0,          // 0 is never played
    BUZZ_JINGLE,            // START jingle (setup.c)
    BUZZ_WRONG,             // wrong card / wrong position
    BUZZ_MATCH_STANDARD,    // sound profile 0
    BUZZ_MATCH_MARIO,       // sound profile 1 (short match jingle)
    BUZZ_MATCH_PIANO,       // sound profile 2
    BUZZ_DJ_1,
    BUZZ_DJ_2,
    BUZZ_DJ_3,
    BUZZ_DJ_4,
    BUZZ_DJ_5,
    BUZZ_DJ_6,
    BUZZ_FART_1,            // Panel 1: Classic Long Rip
    BUZZ_FART_2,            // Panel 2: High Pinched Squeaker
    BUZZ_FART_3,            // Panel 3: Wet Choppy Sputter
    BUZZ_FART_4,            // Panel 4: Machine-Gun Putter
    BUZZ_FART_5,            // Panel 5: Deep Foghorn Rumble
    BUZZ_FART_6,            // Panel 6: The "Question Mark" Squeak
    BUZZ_MARIO_SLICE        // 20-second Mario theme slice for DJ mode double-tap
} buzz_id_t;

bool     buzzer_start(void);
void     buzzer_play(buzz_id_t id);
void     buzzer_play_mario_slice(uint8_t start_idx, uint8_t end_idx);
uint8_t  buzzer_mario_note_count(void);
uint32_t buzzer_mario_slice_ms(uint8_t start_idx, uint8_t end_idx);