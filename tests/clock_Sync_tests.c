/* Host test for the sync wire formats and the shared-clock estimator.
 * Pure C: no Pico SDK, no FreeRTOS. Build: see tests/run_host_tests.ps1 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wire_format.h"
#include "clock_sync_core.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void test_wire(void) {
    uint8_t b[5]; uint8_t seq, anim; uint32_t v; uint16_t d;

    wire_tick_encode(0xA, 0x12345678u, b);
    CHECK(b[0] == 0xA0 && b[1] == 0x12 && b[2] == 0x34 && b[3] == 0x56 && b[4] == 0x78);
    CHECK(wire_tick_decode(b, 5, &seq, &v) && seq == 0xA && v == 0x12345678u);
    CHECK(!wire_tick_decode(b, 4, &seq, &v));          /* wrong length */
    CHECK(!wire_tick_decode(b, 6, &seq, &v));
    b[0] |= 1; CHECK(!wire_tick_decode(b, 5, &seq, &v)); /* spare bit set */

    wire_play_encode(3, 0xF, 0xDEADBEEFu, b);
    CHECK(b[0] == (3 << 6 | 0xF << 2));
    CHECK(wire_play_decode(b, 5, &anim, &seq, &v) && anim == 3 && seq == 0xF && v == 0xDEADBEEFu);
    b[0] |= 2; CHECK(!wire_play_decode(b, 5, &anim, &seq, &v));
    CHECK(!wire_play_decode(NULL, 5, &anim, &seq, &v));

    wire_req_encode(2, 1500, b);
    CHECK(b[0] == 0x80 && b[1] == 0x05 && b[2] == 0xDC);
    CHECK(wire_req_decode(b, 3, &anim, &d) && anim == 2 && d == 1500);
    b[0] |= 1; CHECK(!wire_req_decode(b, 3, &anim, &d));
    CHECK(!wire_req_decode(b, 5, &anim, &d));

    for (uint8_t a = 0; a < 4; ++a) {                  /* every animation ID survives */
        wire_play_encode(a, 7, 1, b);
        CHECK(wire_play_decode(b, 5, &anim, &seq, &v) && anim == a);
    }
}

static void test_min_filter(void) {
    cs_state_t cs; cs_init(&cs);
    /* true offset 1000, delays 5000,3000,9000,2000 -> samples 6000,4000,10000,3000 */
    uint32_t delays[] = {5000, 3000, 9000, 2000};
    CHECK(!cs_valid(&cs, 0));
    for (int i = 0; i < 4; ++i) {
        uint32_t master = 100000u * (uint32_t)(i + 1);
        cs_on_tick(&cs, master, master + 1000 + delays[i]);
    }
    CHECK(cs.offset_us == 3000);                       /* offset + smallest delay */
    CHECK(cs.spread_us == 7000);                       /* 10000 - 3000 */
    CHECK(cs_valid(&cs, 400000 + 1000));
    CHECK(cs_valid(&cs, 403000 + CS_STALE_US - 5));
    CHECK(!cs_valid(&cs, 403000 + CS_STALE_US + 5));   /* last tick arrived at 403000: stale after 3 s */
}

static void test_not_valid_until_enough_samples(void) {
    cs_state_t cs; cs_init(&cs);
    cs_on_tick(&cs, 0, 100);       CHECK(!cs_valid(&cs, 100));
    cs_on_tick(&cs, 1000, 1100);   CHECK(!cs_valid(&cs, 1100));
    cs_on_tick(&cs, 2000, 2100);   CHECK(cs_valid(&cs, 2100));
}

static void test_wraparound(void) {
    cs_state_t cs; cs_init(&cs);
    /* master counter about to wrap; node booted much later so its counter is small */
    uint32_t m = 0xFFFFF000u, off = 0x00ABCDEFu;
    for (int i = 0; i < 5; ++i) { cs_on_tick(&cs, m, m + off + 2000); m += 1000000u; }
    CHECK(cs.offset_us == off + 2000);
    uint32_t start_master = m + 500000u;               /* crosses the 2^32 boundary */
    uint32_t now_local = (m - 1000000u) + off + 2000 + 100;
    int32_t until = cs_us_until(&cs, start_master, now_local);
    CHECK(until == (int32_t)(start_master - (m - 1000000u) - 100));
    CHECK(cs_local_to_master(&cs, cs_master_to_local(&cs, start_master)) == start_master);
}

static void test_spike_and_reboot(void) {
    cs_state_t cs; cs_init(&cs);
    uint32_t m = 0;
    for (int i = 0; i < 5; ++i) { m += 1000000u; cs_on_tick(&cs, m, m + 4000); }
    CHECK(cs.offset_us == 4000);

    m += 1000000u;                                      /* one 300 ms Wi-Fi stall: ignored */
    CHECK(!cs_on_tick(&cs, m, m + 300000));
    CHECK(cs.offset_us == 4000 && cs.resets == 0);
    m += 1000000u; cs_on_tick(&cs, m, m + 4100);        /* back to normal clears the streak */
    CHECK(cs.jump_streak == 0);

    for (int i = 0; i < 3; ++i) { m += 1000000u; cs_on_tick(&cs, m, m + 5000000u); }
    CHECK(cs.resets == 1 && cs.offset_us == 5000000u);  /* 3 high samples in a row = a real step */

    /* Master reboots: its counter restarts near 0, so rx - master jumps UP. Needs the streak. */
    cs_state_t c2; cs_init(&c2);
    for (int i = 0; i < 5; ++i) { uint32_t mm = 900000000u + (uint32_t)i * 1000000u; cs_on_tick(&c2, mm, mm + 4000); }
    uint32_t rx = 900000000u + 6000000u;
    for (int i = 0; i < 3; ++i) { cs_on_tick(&c2, 250000u * (uint32_t)i, rx + 250000u * (uint32_t)i + 4000); }
    CHECK(c2.resets == 1);
    CHECK(c2.offset_us == rx + 4000);
    CHECK(c2.count == 1);                               /* window restarted from the new timeline */

    /* A sample far BELOW the estimate cannot be a delay (delay >= 0): reset at once. */
    cs_state_t c3; cs_init(&c3);
    for (int i = 0; i < 4; ++i) cs_on_tick(&c3, (uint32_t)i * 1000000u, (uint32_t)i * 1000000u + 9000000u);
    cs_on_tick(&c3, 4000000u, 4000000u + 100u);
    CHECK(c3.resets == 1 && c3.offset_us == 100u);
}

/* Simulation: 4 nodes with random boot offsets, 50 ppm crystals, 3 ms floor + exponential
 * jitter (mean 4 ms), master loops its own tick back like any node. Show start in 600 ms.
 * This validates the algorithm. It is NOT a hardware measurement. */
static double urand(void) { return (rand() + 1.0) / (RAND_MAX + 2.0); }
static double delay_us(void) { return 3000.0 - 4000.0 * log(urand()); }

typedef struct { double boot_off, drift; cs_state_t cs; } simnode_t;

static uint32_t local_of(const simnode_t *n, double t_us) {          /* true time -> node counter */
    return (uint32_t)(int64_t)((t_us * (1.0 + n->drift)) + n->boot_off);
}
static double true_of(const simnode_t *n, uint32_t local_us, double near_true) {   /* inverse, mod-safe */
    double est = (local_us - n->boot_off) / (1.0 + n->drift);
    double period = 4294967296.0 / (1.0 + n->drift);
    while (est < near_true - period / 2) est += period;
    while (est > near_true + period / 2) est -= period;
    return est;
}

static void test_simulation(void) {
    srand(7);
    simnode_t n[4];
    for (int i = 0; i < 4; ++i) {
        n[i].boot_off = (double)(rand() % 100000000) * 10.0;           /* up to ~1000 s apart */
        n[i].drift = ((rand() % 101) - 50) * 1e-6;
        cs_init(&n[i].cs);
    }
    double worst = 0, worst_after_lock = 0;
    for (int round = 0; round < 40; ++round) {
        double t = 1e6 * (round + 1);                                  /* master sends at true t */
        uint32_t stamp = local_of(&n[0], t);
        for (int i = 0; i < 4; ++i) cs_on_tick(&n[i].cs, stamp, local_of(&n[i], t + delay_us()));
        if (round < 10) continue;
        /* command: start 600 ms ahead on the master timeline */
        uint32_t start_master = stamp + 600000u;
        double fire[4], mn = 1e18, mx = -1e18;
        for (int i = 0; i < 4; ++i) {
            uint32_t target = cs_master_to_local(&n[i].cs, start_master);
            fire[i] = true_of(&n[i], target, t + 600000.0);
            if (fire[i] < mn) mn = fire[i];
            if (fire[i] > mx) mx = fire[i];
        }
        if (mx - mn > worst) worst = mx - mn;
        if (round >= 15 && mx - mn > worst_after_lock) worst_after_lock = mx - mn;
    }
    printf("sim: worst node-to-node fire skew over 30 shows = %.0f us (after warm-up %.0f us)\n", worst, worst_after_lock);
    CHECK(worst < 10000.0);        /* well under the 10 ms mean jitter fed in; min filter at work */
}

int main(void) {
    test_wire();
    test_min_filter();
    test_not_valid_until_enough_samples();
    test_wraparound();
    test_spike_and_reboot();
    test_simulation();
    if (failures) { printf("clock_sync_test: %d FAILED\n", failures); return 1; }
    printf("clock_sync_test: all passed\n");
    return 0;
}