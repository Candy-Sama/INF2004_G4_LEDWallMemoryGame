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

// Match sounds (one per sound profile)
static const note_t MATCH_STANDARD[] = {{880,100},{1175,160}};
static const note_t MATCH_MARIO[]    = {{659,90},{659,90},{0,60},{659,90},{0,60},{523,90},{659,90},{784,200}};
static const note_t MATCH_PIANO[]    = {{523,80},{659,80},{784,80},{1047,160}};

// Louder 8-Bit DJ pads (tuned to the piezo buzzer's resonant range)
static const note_t DJ_KICK[]  = {{2000,20},{900,30},{450,40},{260,50}};       // Panel 1: Punchy kick drop
static const note_t DJ_HAT[]   = {{3600,20},{4200,20},{3200,30}};                 // Panel 2: Crisp metallic hi-hat
static const note_t DJ_SNARE[] = {{2400,25},{1100,25},{1900,25},{750,45}};        // Panel 3: Sharp snare snap
static const note_t DJ_SYNTH[] = {{1568,55},{2093,95}};                           // Panel 4: Bright synth stab
static const note_t DJ_BASS[]  = {{659,65},{440,95}};                             // Panel 5: Audible arcade bass
static const note_t DJ_LASER[] = {{3200,25},{2400,25},{1600,25},{900,40}};        // Panel 6: Loud laser sweep

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct { buzz_id_t id; const note_t *notes; size_t count; } sound_t;

static const sound_t SOUNDS[] = {
    { BUZZ_JINGLE,         JINGLE,         COUNT(JINGLE) },
    { BUZZ_WRONG,          WRONG,          COUNT(WRONG) },
    { BUZZ_MATCH_STANDARD, MATCH_STANDARD, COUNT(MATCH_STANDARD) },
    { BUZZ_MATCH_MARIO,    MATCH_MARIO,    COUNT(MATCH_MARIO) },
    { BUZZ_MATCH_PIANO,    MATCH_PIANO,    COUNT(MATCH_PIANO) },
    { BUZZ_DJ_1,           DJ_KICK,        COUNT(DJ_KICK) },
    { BUZZ_DJ_2,           DJ_HAT,         COUNT(DJ_HAT) },
    { BUZZ_DJ_3,           DJ_SNARE,       COUNT(DJ_SNARE) },
    { BUZZ_DJ_4,           DJ_SYNTH,       COUNT(DJ_SYNTH) },
    { BUZZ_DJ_5,           DJ_BASS,        COUNT(DJ_BASS) },
    { BUZZ_DJ_6,           DJ_LASER,       COUNT(DJ_LASER) },
};

static QueueHandle_t buzz_q;
static uint slice;

static void tone(uint16_t hz) {
    if (hz == 0) { pwm_set_gpio_level(BUZZER_PIN, 0); return; }
    uint32_t wrap = 1000000u / hz - 1;               // 1 MHz PWM clock
    pwm_set_wrap(slice, (uint16_t)wrap);
    pwm_set_gpio_level(BUZZER_PIN, (uint16_t)(wrap / 2));
}

static void play(const note_t *n, size_t count) {
    for (size_t i = 0; i < count; i++) {
        tone(n[i].hz);
        vTaskDelay(pdMS_TO_TICKS(n[i].ms));          // only this task waits
    }
    tone(0);
}

static void buzzer_task(void *arg) {
    (void)arg;
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_clkdiv(slice, (float)clock_get_hz(clk_sys) / 1000000.0f);
    pwm_set_enabled(slice, true);
    tone(0);
    uint8_t id;
    for (;;) {
        if (xQueueReceive(buzz_q, &id, portMAX_DELAY) != pdTRUE) continue;
        for (size_t i = 0; i < COUNT(SOUNDS); i++) {
            if (SOUNDS[i].id == id) { play(SOUNDS[i].notes, SOUNDS[i].count); break; }
        }
    }
}

void buzzer_play(buzz_id_t id) {
    uint8_t v = (uint8_t)id;
    if (buzz_q) xQueueSend(buzz_q, &v, 0);
}

bool buzzer_start(void) {
    buzz_q = xQueueCreate(8, sizeof(uint8_t));       // deeper queue so quick DJ taps aren't lost
    if (!buzz_q) return false;
    return xTaskCreate(buzzer_task, "buzzer", 512, NULL, 1, NULL) == pdPASS;
}