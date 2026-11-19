#ifndef MOTION_DETECTOR_H
#define MOTION_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "node_config.h"

/* Tap classifier: counts taps inside a window and reports single/double/triple. */
#ifndef TAP_GAP_MS
#define TAP_GAP_MS 400u     // window after the latest tap before the sequence is closed
#endif

typedef enum {
    TAP_NONE = 0,
    TAP_SINGLE,
    TAP_DOUBLE,
    TAP_TRIPLE
} tap_result_t;

typedef struct {
    bool     pending;       // a sequence is in progress
    uint8_t  count;         // taps so far
    uint32_t last_tap_ms;   // time of the latest tap
} tap_classifier_t;

static inline tap_result_t tap_classifier_update(tap_classifier_t *s, bool valid_tap, uint32_t now_ms) {
    if (valid_tap) {
        if (!s->pending) { s->pending = true; s->count = 0; }
        s->count++;
        s->last_tap_ms = now_ms;
        if (s->count >= 3) {                 // third tap: report immediately
            s->pending = false;
            s->count = 0;
            return TAP_TRIPLE;
        }
        return TAP_NONE;
    }
    if (s->pending && (uint32_t)(now_ms - s->last_tap_ms) >= TAP_GAP_MS) {
        uint8_t n = s->count;                // window closed: report what we saw
        s->pending = false;
        s->count = 0;
        return n == 1 ? TAP_SINGLE : TAP_DOUBLE;
    }
    return TAP_NONE;
}
/* End of tap detection code */


/* End of double tap detection code */

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
    /* Debug 
    if (!state->armed && largest_change >= MOTION_THRESHOLD_MG) {
        printf("[%lu ms] BLOCKED: change=%d mg, quiet=%lu ms, gap=%lu ms\n",
            (unsigned long)now_ms,
            largest_change,
            (unsigned long)(now_ms - state->quiet_since_ms),
            (unsigned long)(now_ms - state->last_trigger_ms));
    }
    */

    if (state->armed && largest_change >= MOTION_THRESHOLD_MG) { //If this is armed, AND its not more than the threshold. 
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
