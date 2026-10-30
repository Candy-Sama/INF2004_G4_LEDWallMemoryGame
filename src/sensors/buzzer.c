// buzzer.c
#include "buzzer.h"
#include <stdint.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#define BUZZER_PIN 18 //buzzer pin on maker pi pico

typedef struct { uint16_t hz, ms; } note_t;   // hz 0 = rest
static const note_t JINGLE[] = {{523,120},{659,120},{784,120},{1047,250},{0,60},{784,120},{1047,350}};
static const note_t WRONG[]  = {{220,200},{0,50},{220,300}};

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
        if (id == BUZZ_JINGLE) play(JINGLE, sizeof(JINGLE) / sizeof(JINGLE[0]));
        else if (id == BUZZ_WRONG) play(WRONG, sizeof(WRONG) / sizeof(WRONG[0]));
    }
}

void buzzer_play(buzz_id_t id) { uint8_t v = (uint8_t)id; xQueueSend(buzz_q, &v, 0); }

bool buzzer_start(void) {
    buzz_q = xQueueCreate(4, sizeof(uint8_t));
    if (!buzz_q) return false;
    return xTaskCreate(buzzer_task, "buzzer", 512, NULL, 1, NULL) == pdPASS;
}