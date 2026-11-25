// buzzer.c
#include "buzzer.h"
#include <stdint.h>
#include <stddef.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#define BUZZER_PIN 18   // onboard buzzer on the Maker Pi Pico

typedef struct { uint16_t hz, ms; } note_t;   // hz 0 = rest

static const note_t JINGLE[] = {{523,120},{659,120},{784,120},{1047,250},{0,60},{784,120},{1047,350}};
static const note_t WRONG[]  = {{220,200},{0,50},{220,300}};

// Short match sounds for Memory Game
static const note_t MATCH_STANDARD[] = {{880,100},{1175,160}};
static const note_t MATCH_MARIO[]    = {{659,90},{659,90},{0,60},{659,90},{0,60},{523,90},{659,90},{784,200}};
static const note_t MATCH_PIANO[]    = {{523,80},{659,80},{784,80},{1047,160}};

// Louder 8-Bit DJ pads
static const note_t DJ_KICK[]  = {{2000,20},{900,30},{450,40},{260,50}};
static const note_t DJ_HAT[]   = {{3600,20},{4200,20},{3200,30}};
static const note_t DJ_SNARE[] = {{2400,25},{1100,25},{1900,25},{750,45}};
static const note_t DJ_SYNTH[] = {{1568,55},{2093,95}};
static const note_t DJ_BASS[]  = {{659,65},{440,95}};
static const note_t DJ_LASER[] = {{3200,25},{2400,25},{1600,25},{900,40}};

// 6 Realistic Piezo Fart Sounds (8 ms buzz/rest chopping + downward pitch droop)
static const note_t FART_CLASSIC[] = {
    {520,10},{0,7},  {480,8},{0,8},   {450,8},{0,8},   {420,8},{0,8},
    {390,9},{0,8},   {360,9},{0,9},   {330,10},{0,9},  {300,10},{0,10},
    {275,12},{0,10}, {250,14},{0,12}, {230,22}
};
static const note_t FART_SQUEAKER[] = {
    {620,12},{0,7},  {780,14},{0,7},  {960,18},{0,7},  {1120,24},{0,8},
    {1040,16},{0,7}, {860,14},{0,8},  {640,14},{0,9},  {440,22}
};
static const note_t FART_SPUTTER[] = {
    {440,8},{0,8},   {360,9},{0,32},
    {420,8},{0,8},   {340,8},{0,8},   {290,10},{0,42},
    {380,8},{0,8},   {310,9},{0,28},
    {330,9},{0,9},   {260,14}
};
static const note_t FART_MACHINE_GUN[] = {
    {450,8},{0,7},   {340,10},{0,36},
    {440,8},{0,7},   {330,10},{0,36},
    {420,8},{0,7},   {310,10},{0,36},
    {400,8},{0,7},   {290,10},{0,36},
    {370,9},{0,8},   {260,16}
};
static const note_t FART_FOGHORN[] = {
    {340,9},{250,9}, {335,9},{245,9}, {325,10},{240,10}, {315,10},{235,10},
    {300,11},{225,11},{285,12},{215,12},{270,14},{205,14}, {250,18},{195,24}
};
static const note_t FART_QUESTION[] = {
    {390,8},{0,8},   {350,8},{0,8},   {320,9},{0,9},   {300,10},{0,12},
    {420,14},{560,16},{740,20},{950,35}
};

// First 30 seconds (30,000 ms exact) of Super Mario Bros Overworld Theme
static const note_t MARIO_THEME[] = {
    // Intro (3,200 ms)
    {659,150},{0,50}, {659,150},{0,250}, {659,150},{0,250},
    {523,180},{0,20}, {659,180},{0,220}, {784,250},{0,550}, {392,250},{0,550},
    // Main phrase A1 (3,200 ms)
    {523,250},{0,350}, {392,250},{0,350}, {330,250},{0,350},
    {440,220},{0,180}, {494,220},{0,180}, {466,180},{0,20}, {440,250},{0,150},
    // Main phrase B1 (3,200 ms)
    {392,240},{0,30},  {659,240},{0,30},  {784,230},{0,30},
    {880,220},{0,180}, {698,180},{0,20},  {784,180},{0,220},
    {659,220},{0,180}, {523,180},{0,20},  {587,180},{0,20},  {494,250},{0,350},
    // Main phrase A2 (3,200 ms)
    {523,250},{0,350}, {392,250},{0,350}, {330,250},{0,350},
    {440,220},{0,180}, {494,220},{0,180}, {466,180},{0,20}, {440,250},{0,150},
    // Main phrase B2 (3,200 ms)
    {392,240},{0,30},  {659,240},{0,30},  {784,230},{0,30},
    {880,220},{0,180}, {698,180},{0,20},  {784,180},{0,220},
    {659,220},{0,180}, {523,180},{0,20},  {587,180},{0,20},  {494,250},{0,350},
    // Bridge 1 (4,000 ms)
    {0,400},
    {784,180},{0,20},  {740,180},{0,20},  {698,180},{0,20},
    {622,220},{0,180}, {659,220},{0,180},
    {415,180},{0,20},  {440,180},{0,20},  {523,180},{0,20},  {0,200},
    {440,180},{0,20},  {523,180},{0,20},  {587,250},{0,150},
    {784,180},{0,20},  {740,180},{0,20},  {698,180},{0,20},
    // Bridge 2 + high C6 flourish (3,600 ms)
    {622,220},{0,180}, {659,220},{0,180},
    {1047,200},{0,200},{1047,180},{0,20}, {1047,250},{0,550},
    {0,200},
    {784,180},{0,20},  {740,180},{0,20},  {698,180},{0,20},
    {622,220},{0,180}, {659,180},{0,20},
    // Final Chorus A3 (3,200 ms)
    {523,250},{0,350}, {392,250},{0,350}, {330,250},{0,350},
    {440,220},{0,180}, {494,220},{0,180}, {466,180},{0,20}, {440,250},{0,150},
    // Final Chorus B3 (3,200 ms)
    {392,240},{0,30},  {659,240},{0,30},  {784,230},{0,30},
    {880,220},{0,180}, {698,180},{0,20},  {784,180},{0,220},
    {659,220},{0,180}, {523,180},{0,20},  {587,180},{0,20},  {494,250},{0,350}
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct { buzz_id_t id; const note_t *notes; size_t count; } sound_t;
typedef struct { uint8_t id; uint8_t start; uint8_t end; uint32_t delay_ms; uint32_t gen; } buzz_req_t;

static const sound_t SOUNDS[] = {
    { BUZZ_JINGLE,         JINGLE,           COUNT(JINGLE) },
    { BUZZ_WRONG,          WRONG,            COUNT(WRONG) },
    { BUZZ_MATCH_STANDARD, MATCH_STANDARD,   COUNT(MATCH_STANDARD) },
    { BUZZ_MATCH_MARIO,    MATCH_MARIO,      COUNT(MATCH_MARIO) },
    { BUZZ_MATCH_PIANO,    MATCH_PIANO,      COUNT(MATCH_PIANO) },
    { BUZZ_DJ_1,           DJ_KICK,          COUNT(DJ_KICK) },
    { BUZZ_DJ_2,           DJ_HAT,           COUNT(DJ_HAT) },
    { BUZZ_DJ_3,           DJ_SNARE,         COUNT(DJ_SNARE) },
    { BUZZ_DJ_4,           DJ_SYNTH,         COUNT(DJ_SYNTH) },
    { BUZZ_DJ_5,           DJ_BASS,          COUNT(DJ_BASS) },
    { BUZZ_DJ_6,           DJ_LASER,         COUNT(DJ_LASER) },
    { BUZZ_FART_1,         FART_CLASSIC,     COUNT(FART_CLASSIC) },
    { BUZZ_FART_2,         FART_SQUEAKER,    COUNT(FART_SQUEAKER) },
    { BUZZ_FART_3,         FART_SPUTTER,     COUNT(FART_SPUTTER) },
    { BUZZ_FART_4,         FART_MACHINE_GUN, COUNT(FART_MACHINE_GUN) },
    { BUZZ_FART_5,         FART_FOGHORN,     COUNT(FART_FOGHORN) },
    { BUZZ_FART_6,         FART_QUESTION,    COUNT(FART_QUESTION) },
};

static QueueHandle_t buzz_q;
static uint slice;
static volatile bool     s_pwm_ready = false;
static volatile uint32_t s_buzz_gen  = 0;

uint8_t buzzer_mario_note_count(void) {
    return (uint8_t)COUNT(MARIO_THEME);
}

uint32_t buzzer_mario_slice_ms(uint8_t start_idx, uint8_t end_idx) {
    uint8_t total = (uint8_t)COUNT(MARIO_THEME);
    if (start_idx >= total) return 0;
    if (end_idx > total) end_idx = total;
    uint32_t sum = 0;
    for (uint8_t i = start_idx; i < end_idx; i++) {
        sum += MARIO_THEME[i].ms;
    }
    return sum;
}

static void tone(uint16_t hz) {
    if (!s_pwm_ready) return;
    if (hz == 0) { pwm_set_gpio_level(BUZZER_PIN, 0); return; }
    uint32_t wrap = 1000000u / hz - 1;               // 1 MHz PWM clock
    pwm_set_wrap(slice, (uint16_t)wrap);
    pwm_set_gpio_level(BUZZER_PIN, (uint16_t)(wrap / 2));
}

static void play(const note_t *n, size_t count, uint32_t gen) {
    TickType_t slice_start = xTaskGetTickCount();
    uint32_t   cum_ms      = 0;

    for (size_t i = 0; i < count; i++) {
        if (s_buzz_gen != gen) break;

        uint32_t dur_ms  = n[i].ms;
        uint32_t gap_ms  = (n[i].hz > 0 && dur_ms >= 60) ? 15 : 0;
        uint32_t tone_ms = dur_ms - gap_ms;

        tone(n[i].hz);
        TickType_t tone_end = slice_start + pdMS_TO_TICKS(cum_ms + tone_ms);
        while (s_buzz_gen == gen && (int32_t)(tone_end - xTaskGetTickCount()) > 0) {
            TickType_t rem = tone_end - xTaskGetTickCount();
            vTaskDelay(rem > pdMS_TO_TICKS(20) ? pdMS_TO_TICKS(20) : rem);
        }

        if (gap_ms > 0 && s_buzz_gen == gen) {
            tone(0);
            TickType_t note_end = slice_start + pdMS_TO_TICKS(cum_ms + dur_ms);
            while (s_buzz_gen == gen && (int32_t)(note_end - xTaskGetTickCount()) > 0) {
                TickType_t rem = note_end - xTaskGetTickCount();
                vTaskDelay(rem > pdMS_TO_TICKS(20) ? pdMS_TO_TICKS(20) : rem);
            }
        }

        cum_ms += dur_ms;
    }
    tone(0);
}

static void buzzer_task(void *arg) {
    (void)arg;
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_clkdiv(slice, (float)clock_get_hz(clk_sys) / 1000000.0f);
    pwm_set_enabled(slice, true);
    s_pwm_ready = true;
    tone(0);

    buzz_req_t req;
    for (;;) {
        if (xQueueReceive(buzz_q, &req, portMAX_DELAY) != pdTRUE) continue;

        if (req.delay_ms > 0) {
            TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(req.delay_ms);
            while (s_buzz_gen == req.gen && (int32_t)(deadline - xTaskGetTickCount()) > 0) {
                TickType_t rem = deadline - xTaskGetTickCount();
                vTaskDelay(rem > pdMS_TO_TICKS(20) ? pdMS_TO_TICKS(20) : rem);
            }
        }
        if (s_buzz_gen != req.gen) continue;

        if (req.id == BUZZ_MARIO_SLICE) {
            uint8_t total = (uint8_t)COUNT(MARIO_THEME);
            uint8_t s = req.start < total ? req.start : total;
            uint8_t e = req.end <= total ? req.end : total;
            if (e > s) {
                play(&MARIO_THEME[s], (size_t)(e - s), req.gen);
            }
            continue;
        }

        for (size_t i = 0; i < COUNT(SOUNDS); i++) {
            if (SOUNDS[i].id == req.id) {
                play(SOUNDS[i].notes, SOUNDS[i].count, req.gen);
                break;
            }
        }
    }
}

void buzzer_stop(void) {
    s_buzz_gen++;
    if (buzz_q) xQueueReset(buzz_q);
    tone(0);
}

void buzzer_play(buzz_id_t id) {
    uint32_t gen = ++s_buzz_gen;
    buzz_req_t req = { .id = (uint8_t)id, .start = 0, .end = 0, .delay_ms = 0, .gen = gen };
    if (buzz_q) xQueueSend(buzz_q, &req, 0);
}

void buzzer_play_mario_slice(uint8_t start_idx, uint8_t end_idx) {
    buzzer_play_mario_slice_delayed(start_idx, end_idx, 0);
}

void buzzer_play_mario_slice_delayed(uint8_t start_idx, uint8_t end_idx, uint32_t delay_ms) {
    uint32_t gen = ++s_buzz_gen;
    buzz_req_t req = {
        .id       = (uint8_t)BUZZ_MARIO_SLICE,
        .start    = start_idx,
        .end      = end_idx,
        .delay_ms = delay_ms,
        .gen      = gen
    };
    if (buzz_q) xQueueSend(buzz_q, &req, 0);
}

bool buzzer_start(void) {
    buzz_q = xQueueCreate(8, sizeof(buzz_req_t));
    if (!buzz_q) return false;
    return xTaskCreate(buzzer_task, "buzzer", 512, NULL, 1, NULL) == pdPASS;
}