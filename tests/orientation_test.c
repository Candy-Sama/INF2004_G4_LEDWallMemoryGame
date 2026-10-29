#include <assert.h>
#include <stdio.h>
#include "imu_orientation.h"

// Feed the same pose for `ms` milliseconds at 10 ms intervals.
static bool hold_pose(orientation_detector_t *d, int x, int y, int z,
                      uint32_t start, uint32_t ms) {
    bool changed = false;
    for (uint32_t t = 0; t <= ms; t += 10) {
        changed |= orientation_update(d, x, y, z, start + t);
    }
    return changed;
}

int main(void) {
    orientation_detector_t d;
    orientation_detector_init(&d);
    assert(d.stable == ORIENT_UNKNOWN);

    // Not accepted until the pose has settled.
    assert(!orientation_update(&d, 0, 1000, 0, 0));
    assert(!orientation_update(&d, 0, 1000, 0, 200));
    assert(orientation_update(&d, 0, 1000, 0, 300));
    assert(d.stable == ORIENT_UPRIGHT && d.image_turns == 0);

    // Each side, with the matching compensating image rotation.
    assert(hold_pose(&d, -1000, 0, 0, 1000, 400) && d.stable == ORIENT_ROT_CW90);
    assert(d.image_turns == 3);
    assert(hold_pose(&d, 0, -1000, 0, 2000, 400) && d.stable == ORIENT_UPSIDE_DOWN);
    assert(d.image_turns == 2);
    assert(hold_pose(&d, 1000, 0, 0, 3000, 400) && d.stable == ORIENT_ROT_CCW90);
    assert(d.image_turns == 1);

    // Lying flat is reported but the last upright rotation is kept.
    assert(hold_pose(&d, 0, 0, 1000, 4000, 400) && d.stable == ORIENT_FACE_UP);
    assert(d.image_turns == 1);
    assert(hold_pose(&d, 0, 0, -1000, 5000, 400) && d.stable == ORIENT_FACE_DOWN);
    assert(d.image_turns == 1);

    // Back to upright: flat hysteresis does not block a clear tilt.
    assert(hold_pose(&d, 0, 1000, 100, 6000, 400) && d.stable == ORIENT_UPRIGHT);

    // A brief glitch shorter than the settle time is ignored.
    assert(!hold_pose(&d, 1000, 0, 0, 7000, 150));
    assert(!hold_pose(&d, 0, 1000, 0, 7200, 400));
    assert(d.stable == ORIENT_UPRIGHT);

    // Samples while the board is shaken (magnitude far from 1 g) are ignored.
    assert(!hold_pose(&d, 1500, 1500, 0, 8000, 1000));
    assert(d.stable == ORIENT_UPRIGHT);

    // Near 45 degrees: hysteresis stops flicker between upright and a side.
    assert(!hold_pose(&d, 760, 700, 0, 9000, 1000));
    assert(d.stable == ORIENT_UPRIGHT);
    assert(hold_pose(&d, 900, 400, 0, 11000, 400) && d.stable == ORIENT_ROT_CCW90);

    // Slightly-off-flat pose with no clear side stays put.
    orientation_detector_init(&d);
    assert(hold_pose(&d, 0, 1000, 0, 0, 400) && d.stable == ORIENT_UPRIGHT);
    assert(!hold_pose(&d, 150, 150, 700, 1000, 1000));
    assert(d.stable == ORIENT_UPRIGHT);

    // Timer wrap-around.
    orientation_detector_init(&d);
    assert(hold_pose(&d, 0, 1000, 0, UINT32_MAX - 100, 400) && d.stable == ORIENT_UPRIGHT);

    // Shared-rotation wire format: exactly one byte, 0..3.
    uint8_t turns = 9;
    uint8_t ok[] = {3};
    assert(orientation_sync_decode(ok, 1, &turns) && turns == 3);
    uint8_t bad[] = {4};
    turns = 9;
    assert(!orientation_sync_decode(bad, 1, &turns) && turns == 9);
    assert(!orientation_sync_decode(ok, 0, &turns));
    assert(!orientation_sync_decode(ok, 2, &turns));
    assert(!orientation_sync_decode(NULL, 1, &turns));
    assert(!orientation_sync_decode(ok, 1, NULL));

    puts("orientation tests passed");
    return 0;
}