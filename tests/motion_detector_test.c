#include <assert.h>
#include <stdio.h>
#include "motion_detector.h"

static void settle(motion_detector_t *s, int x, uint32_t start, unsigned duration) {
    for (unsigned t = 0; t <= duration; t += 10) {
        assert(!motion_detector_update(s, x, 0, 1000, start + t));
    }
}

int main(void) {
    motion_detector_t s = {0};
    // Gravity on startup is not a movement event; quiet samples arm the detector.
    settle(&s, 0, 0, 200);
    assert(s.armed);
    assert(motion_detector_update(&s, 300, 0, 1000, 210));
    // The return swing of the same movement is suppressed.
    assert(!motion_detector_update(&s, -300, 0, 1000, 220));
    settle(&s, -300, 230, 770);
    assert(!s.armed);
    assert(!motion_detector_update(&s, -300, 0, 1000, 1010));
    assert(s.armed);
    assert(motion_detector_update(&s, 0, 0, 1000, 1020));

    // Sustained shaking cannot rearm even after the cooldown has elapsed.
    for (unsigned t = 1030; t <= 3030; t += 10) {
        int x = (t / 10) % 2 ? 500 : -500;
        assert(!motion_detector_update(&s, x, 0, 1000, t));
    }
    assert(!s.armed);

    // A long gap resets the baseline instead of interpreting an old sample.
    settle(&s, 0, 4000, 200);
    assert(s.armed);
    assert(!motion_detector_update(&s, 1500, 0, 1000, 5000));
    assert(!s.armed);

    // Unsigned elapsed-time arithmetic handles the millisecond timer wrapping.
    motion_detector_t wrap = {0};
    uint32_t start = UINT32_MAX - 100;
    settle(&wrap, 0, start, 200);
    assert(motion_detector_update(&wrap, 300, 0, 1000, start + 210));
    puts("Motion detector tests passed.");
}
