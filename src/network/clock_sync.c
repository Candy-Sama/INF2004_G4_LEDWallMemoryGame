// clock_sync.c : shared clock + scheduled animation start (MQTT add-on, Member 1)
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "clock_sync.h"
#include "clock_sync_core.h"
#include "wire_format.h"
#include "net.h"
#include "mqtt_handler.h"
#include "node_identity.h"
#include "display.h"

/* Must be ABOVE the tcpip thread (TCPIP_THREAD_PRIO 5 in lwipopts.h), the Wi-Fi context (4) and
 * display (4). At equal priority time slicing could interrupt the final busy-wait and add up to
 * one tick of jitter. Cost: lwIP can be held off for at most SYNC_SPIN_US (1.5 ms) per show. */
#define SYNC_TASK_PRIORITY 6
#define SYNC_TASK_STACK    1024  /* words */
#define SYNC_PLAY_QUEUE    4

typedef struct {
    uint8_t  anim, seq;
    uint32_t start_master_us;
    uint32_t rx_local_us;
} play_evt_t;

static cs_state_t   s_cs;            /* written by net_task, read by sync_task: guarded by a critical section */
static QueueHandle_t s_play_q;
static uint8_t      s_last_tick_seq = 0xFF;
static uint32_t     s_ticks_missed;
static bool         s_have_last_play;
static uint8_t      s_last_play_seq;
static uint32_t     s_last_play_start;

#define LOG(...) do { printf("[SYNC] n=%d ", identity_node_id()); printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---------------------------------------------------------------- state */

static bool get_offset(uint32_t now_us, uint32_t *offset) {
    bool ok;
    taskENTER_CRITICAL();
    ok = cs_valid(&s_cs, now_us);
    if (ok) *offset = s_cs.offset_us;
    taskEXIT_CRITICAL();
    return ok;
}

bool clock_sync_is_synced(void) {
    uint32_t off;
    return get_offset(time_us_32(), &off);
}

/* ------------------------------------------------- hook (override me) */

__attribute__((weak)) void anim_on_play(uint8_t anim_id, uint8_t slot) {
    static const uint8_t colour[WIRE_ANIM_COUNT][3] = {
        {12, 0, 0}, {0, 12, 0}, {0, 0, 12}, {12, 10, 0},   /* A red, B green, C blue, D yellow */
    };
    (void)slot;
    display_show_colour(colour[anim_id & 3][0], colour[anim_id & 3][1], colour[anim_id & 3][2]);
}

/* ------------------------------------------------- net handlers (net_task) */

static void handle_tick(const uint8_t *p, size_t len, uint32_t rx_us) {
    uint8_t seq; uint32_t master_us;
    if (!wire_tick_decode(p, len, &seq, &master_us)) { LOG("TICK_BAD len=%u", (unsigned)len); return; }

    if (s_last_tick_seq != 0xFF) s_ticks_missed += (uint8_t)((seq - s_last_tick_seq - 1) & 0xF);
    s_last_tick_seq = seq;

    taskENTER_CRITICAL();
    bool taken = cs_on_tick(&s_cs, master_us, rx_us);
    cs_state_t snap = s_cs;
    taskEXIT_CRITICAL();

#if SYNC_LOG_TICKS
    LOG("TICK seq=%u master_us=%lu rx_us=%lu offset_us=%lu spread_us=%lu samples=%u %s missed=%lu resets=%lu",
        seq, (unsigned long)master_us, (unsigned long)rx_us, (unsigned long)snap.offset_us,
        (unsigned long)snap.spread_us, snap.count, taken ? "ok" : "IGNORED_SPIKE",
        (unsigned long)s_ticks_missed, (unsigned long)snap.resets);
#else
    (void)taken; (void)snap;
#endif
}

static void handle_play(const uint8_t *p, size_t len, uint32_t rx_us) {
    play_evt_t e;
    if (!wire_play_decode(p, len, &e.anim, &e.seq, &e.start_master_us)) { LOG("PLAY_BAD len=%u", (unsigned)len); return; }
    e.rx_local_us = rx_us;

    if (s_have_last_play && s_last_play_seq == e.seq && s_last_play_start == e.start_master_us) {
        LOG("PLAY_DUP seq=%u ignored", e.seq);          /* QoS 1 may deliver twice */
        return;
    }
    s_have_last_play = true; s_last_play_seq = e.seq; s_last_play_start = e.start_master_us;

    uint32_t off;
    if (get_offset(rx_us, &off)) {
        LOG("PLAY_RX seq=%u id=%c start_master_us=%lu rx_master_us=%lu lead_us=%ld rx_us=%lu",
            e.seq, 'A' + e.anim, (unsigned long)e.start_master_us,
            (unsigned long)(rx_us - off), (long)(int32_t)(e.start_master_us + off - rx_us),
            (unsigned long)rx_us);
    } else {
        LOG("PLAY_RX seq=%u id=%c NOT_SYNCED (dropped)", e.seq, 'A' + e.anim);
        return;
    }
    if (xQueueSend(s_play_q, &e, 0) != pdTRUE) LOG("PLAY_QUEUE_FULL seq=%u dropped", e.seq);
}

static void handle_req(const uint8_t *p, size_t len) {
    if (!IS_SYNC_MASTER) return;                         /* only the clock master stamps start times */
    static uint8_t seq;
    uint8_t anim; uint16_t delay_ms;
    if (!wire_req_decode(p, len, &anim, &delay_ms)) { LOG("REQ_BAD len=%u", (unsigned)len); return; }
    if (delay_ms < SYNC_MIN_LEAD_MS) { LOG("REQ delay %u ms too short, using %d", delay_ms, SYNC_MIN_LEAD_MS); delay_ms = SYNC_MIN_LEAD_MS; }
    if (delay_ms > WIRE_REQ_DELAY_MAX_MS) delay_ms = WIRE_REQ_DELAY_MAX_MS;

    uint32_t start = time_us_32() + (uint32_t)delay_ms * 1000u;     /* master timeline = master's own counter */
    uint8_t out[WIRE_PLAY_LEN];
    wire_play_encode(anim, seq, start, out);
    if (net_publish(MQTT_SYNC_PLAY_TOPIC, out, sizeof out, false)) {
        LOG("REQ id=%c delay_ms=%u -> PLAY seq=%u start_master_us=%lu", 'A' + anim, delay_ms, seq, (unsigned long)start);
        seq = (uint8_t)((seq + 1) & 0xF);
    } else {
        LOG("REQ publish failed (queue full)");
    }
}

static void on_sync(const char *topic, const uint8_t *p, size_t len) {
    uint32_t rx_us = net_rx_time_us();                   /* stamped inside the lwIP callback, not at the 10 ms poll */
    const char *leaf = strrchr(topic, '/');
    if (!leaf) return;
    ++leaf;
    if      (strcmp(leaf, "tick") == 0) handle_tick(p, len, rx_us);
    else if (strcmp(leaf, "play") == 0) handle_play(p, len, rx_us);
    else if (strcmp(leaf, "req")  == 0) handle_req(p, len);
}

/* Clock master: publish the tick. Called from net_task right before drain_publish_queue
 * so the stamp is taken as late as possible. QoS 0, no retain. */
void clock_sync_poll(void) {
    if (!IS_SYNC_MASTER) return;
    static uint32_t next_ms; static uint8_t seq, sent;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now_ms - next_ms) < 0) return;

    uint8_t buf[WIRE_TICK_LEN];
    wire_tick_encode(seq, time_us_32(), buf);            /* stamp immediately before the publish call */
    if (mqtt_send_ex(MQTT_SYNC_TICK_TOPIC, buf, sizeof buf, 0, false, NULL, NULL)) {
        seq = (uint8_t)((seq + 1) & 0xF);
        if (sent < 255) ++sent;
        next_ms = now_ms + (sent < 8 ? 250 : 1000);      /* quick lock after boot, then 1 Hz */
    } else {
        next_ms = now_ms + 100;
    }
}

/* ----------------------------------------------------- scheduled fire */

static void pulse_high(void) { if (SYNC_PULSE_GPIO >= 0) gpio_put(SYNC_PULSE_GPIO, 1); }
static void pulse_low(void)  { if (SYNC_PULSE_GPIO >= 0) gpio_put(SYNC_PULSE_GPIO, 0); }

static void run_play(const play_evt_t *e) {
    uint32_t off = 0, now;
    int32_t until;

    for (;;) {                                           /* re-read the offset each pass: ticks keep refining it */
        now = time_us_32();
        if (!get_offset(now, &off)) { LOG("PLAY seq=%u skipped: clock not synced", e->seq); return; }
        until = (int32_t)(e->start_master_us + off - now);
        if (until > SYNC_MAX_LEAD_US) { LOG("PLAY seq=%u skipped: start too far (%ld us)", e->seq, (long)until); return; }
        if (until <= (int32_t)SYNC_SPIN_US) break;
        uint32_t sleep_ms = ((uint32_t)until - SYNC_SPIN_US) / 1000u;
        if (sleep_ms > 100u) sleep_ms = 100u;
        vTaskDelay(sleep_ms ? pdMS_TO_TICKS(sleep_ms) : 1);
    }
    if (until < -(int32_t)SYNC_LATE_TOL_US) {
        LOG("PLAY seq=%u LATE by %ld us, skipped (stale start is never replayed)", e->seq, (long)-until);
        return;
    }

    uint32_t target = e->start_master_us + off;
    while ((int32_t)(time_us_32() - target) < 0) tight_loop_contents();
    uint32_t fired = time_us_32();
    pulse_high();
    anim_on_play(e->anim, (uint8_t)(identity_node_id() > 0 ? identity_node_id() - 1 : 0));

    LOG("FIRE seq=%u id=%c start_master_us=%lu fired_master_us=%lu err_us=%ld fired_us=%lu",
        e->seq, 'A' + e->anim, (unsigned long)e->start_master_us, (unsigned long)(fired - off),
        (long)(int32_t)(fired - target), (unsigned long)fired);
    vTaskDelay(pdMS_TO_TICKS(5));
    pulse_low();
}

static void sync_task(void *arg) {
    (void)arg;
    play_evt_t e;
    for (;;) {
        if (xQueueReceive(s_play_q, &e, portMAX_DELAY) == pdTRUE) run_play(&e);
    }
}

bool clock_sync_start(void) {
    cs_init(&s_cs);
    s_play_q = xQueueCreate(SYNC_PLAY_QUEUE, sizeof(play_evt_t));
    if (!s_play_q) return false;
    if (SYNC_PULSE_GPIO >= 0) { gpio_init(SYNC_PULSE_GPIO); gpio_set_dir(SYNC_PULSE_GPIO, GPIO_OUT); gpio_put(SYNC_PULSE_GPIO, 0); }
    if (!net_register_handler(MQTT_SYNC_FILTER, on_sync)) return false;
    return xTaskCreate(sync_task, "sync", SYNC_TASK_STACK, NULL, SYNC_TASK_PRIORITY, NULL) == pdPASS;
}