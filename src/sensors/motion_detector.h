#ifndef MOTION_DETECTOR_H
#define MOTION_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>
#include "node_config.h"

// A movement experiment, not a calibrated panel-tap classifier.
typedef struct {
    bool initialised;
    bool armed;
    bool triggered;
    int previous[3];
    uint32_t last_sample_ms;
    uint32_t quiet_since_ms;
    uint32_t last_trigger_ms;
} motion_detector_t;

static inline bool motion_detector_update(motion_detector_t *state,
                                          int x, int y, int z, uint32_t now_ms) {
    int values[3] = {x, y, z};
    int largest_change = 0;
    bool reset_baseline = !state->initialised ||
                         (uint32_t)(now_ms - state->last_sample_ms) > 100;
    for (int axis = 0; axis < 3; ++axis) {
        int change = values[axis] - state->previous[axis];
        if (change < 0) change = -change;
        if (change > largest_change) largest_change = change;
        state->previous[axis] = values[axis];
    }
    state->last_sample_ms = now_ms;
    if (reset_baseline) {
        state->initialised = true;
        state->armed = false;
        state->quiet_since_ms = now_ms;
        return false;
    }
    if (state->armed && largest_change >= MOTION_THRESHOLD_MG) {
        state->armed = false;
        state->triggered = true;
        state->last_trigger_ms = now_ms;
        state->quiet_since_ms = now_ms;
        return true;
    }
    if (largest_change > MOTION_RELEASE_MG) {
        state->quiet_since_ms = now_ms;
    }
    if (!state->armed &&
        (uint32_t)(now_ms - state->quiet_since_ms) >= MOTION_QUIET_MS &&
        (!state->triggered ||
         (uint32_t)(now_ms - state->last_trigger_ms) >= MOTION_COOLDOWN_MS)) {
        state->armed = true;
    }
    return false;
}

#endif
