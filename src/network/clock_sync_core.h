#ifndef CLOCK_SYNC_CORE_H
#define CLOCK_SYNC_CORE_H

/*
 * Shared-clock estimator. Pure C, no Pico/RTOS calls, so it is host-tested.
 *
 * The clock master publishes its own 32-bit microsecond counter (master_us) once
 * per tick. A node stamps the tick on arrival (rx_us, its own counter) and keeps
 *
 *      sample = rx_us - master_us      (= clock offset + one-way delay)
 *
 * The delay is never negative, so the SMALLEST recent sample is the one with the
 * least delay. offset = min(window). That is a minimum filter, not NTP: it cannot
 * measure the one-way delay, only reject slow packets. What is left over is the
 * smallest delay actually seen, and the clock master loops its OWN tick back
 * through the broker and runs this same code, so every node (master included)
 * carries the same bias. Skew BETWEEN nodes is what the show needs.
 *
 * All arithmetic is modulo 2^32. Boot clocks differ by arbitrary amounts, so an
 * offset is only meaningful modulo 2^32; compare offsets via a signed difference.
 */

#include <stdbool.h>
#include <stdint.h>

#define CS_WINDOW       8            /* ticks kept; 8 s at the 1 s steady rate     */
#define CS_MIN_SAMPLES  3            /* ticks needed before the clock counts as locked */
#define CS_JUMP_US      100000       /* a sample this far from the estimate is suspect */
#define CS_JUMP_STREAK  3            /* consecutive high samples accepted as a real step */
#define CS_STALE_US     3000000      /* no tick for 3 s -> not synced (same as the 3 s heartbeat rule) */

typedef struct {
    uint32_t sample[CS_WINDOW];
    uint8_t  count;                  /* valid samples, 0..CS_WINDOW */
    uint8_t  head;                   /* next write index */
    uint8_t  jump_streak;
    uint32_t offset_us;              /* local = master + offset_us (mod 2^32) */
    uint32_t spread_us;              /* max-min over the window: delay jitter seen */
    uint32_t last_rx_us;             /* local time of the last accepted tick */
    uint32_t resets;                 /* times the window was discarded (master reboot) */
} cs_state_t;

static inline void cs_init(cs_state_t *cs) {
    for (unsigned i = 0; i < sizeof *cs; ++i) ((uint8_t *)cs)[i] = 0;
}

static inline void cs_recompute(cs_state_t *cs) {
    uint32_t ref = cs->sample[(uint8_t)((cs->head + CS_WINDOW - 1) % CS_WINDOW)];
    int32_t lo = 0, hi = 0;
    for (uint8_t i = 0; i < cs->count; ++i) {
        int32_t d = (int32_t)(cs->sample[(uint8_t)((cs->head + CS_WINDOW - 1 - i) % CS_WINDOW)] - ref);
        if (d < lo) lo = d;
        if (d > hi) hi = d;
    }
    cs->offset_us = ref + (uint32_t)lo;
    cs->spread_us = (uint32_t)(hi - lo);
}

static inline void cs_push(cs_state_t *cs, uint32_t s) {
    cs->sample[cs->head] = s;
    cs->head = (uint8_t)((cs->head + 1) % CS_WINDOW);
    if (cs->count < CS_WINDOW) cs->count++;
    cs_recompute(cs);
}

/* Feed one tick. Returns true if the sample entered the window. */
static inline bool cs_on_tick(cs_state_t *cs, uint32_t master_us, uint32_t rx_us) {
    uint32_t s = rx_us - master_us;
    if (cs->count > 0) {
        int32_t diff = (int32_t)(s - cs->offset_us);
        if (diff < -(int32_t)CS_JUMP_US) {
            /* Delay cannot be negative, so the master's clock restarted. */
            cs->count = 0; cs->head = 0; cs->jump_streak = 0; cs->resets++;
        } else if (diff > (int32_t)CS_JUMP_US) {
            /* One slow packet (Wi-Fi retry) is ignored; a streak is a real step. */
            cs->last_rx_us = rx_us;
            if (++cs->jump_streak < CS_JUMP_STREAK) return false;
            cs->count = 0; cs->head = 0; cs->jump_streak = 0; cs->resets++;
        } else {
            cs->jump_streak = 0;
        }
    }
    cs_push(cs, s);
    cs->last_rx_us = rx_us;
    return true;
}

static inline bool cs_valid(const cs_state_t *cs, uint32_t now_local_us) {
    return cs->count >= CS_MIN_SAMPLES && (int32_t)(now_local_us - cs->last_rx_us) < (int32_t)CS_STALE_US;
}

static inline uint32_t cs_master_to_local(const cs_state_t *cs, uint32_t master_us) { return master_us + cs->offset_us; }
static inline uint32_t cs_local_to_master(const cs_state_t *cs, uint32_t local_us)  { return local_us - cs->offset_us; }

/* Signed microseconds from now until a master-timeline instant (negative = past). */
static inline int32_t cs_us_until(const cs_state_t *cs, uint32_t start_master_us, uint32_t now_local_us) {
    return (int32_t)(cs_master_to_local(cs, start_master_us) - now_local_us);
}

#endif /* CLOCK_SYNC_CORE_H */