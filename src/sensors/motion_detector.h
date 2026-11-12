#ifndef MOTION_DETECTOR_H
#define MOTION_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "node_config.h"

/* Variables to determine if none,single and double tap (Mus) */
typedef enum { //Enum means that the variable can only be one of the following values.
    TAP_NONE = 0, //default value, no tap detected
    TAP_SINGLE,
    TAP_DOUBLE
} tap_result_t; //This tap_result_t is a variable type

typedef struct{
    bool pending;
    uint32_t first_tap_ms;
} tap_classifier_t;

static inline tap_result_t tap_classifier_update(tap_classifier_t *state, bool valid_tap, uint32_t now_ms) {
    if (valid_tap && !state->pending){
        state->pending = true;
        state->first_tap_ms = now_ms; //begins the timer
        return TAP_NONE;
    }

    if (state->pending && (uint32_t)(now_ms - state->first_tap_ms) >= 400u) { //if the timer went past 400ms
        state->pending = valid_tap;
        if (valid_tap) {
            state->first_tap_ms = now_ms; //reset the timer
        }
        return TAP_SINGLE;
    }
    else if (state->pending && valid_tap &&
            (uint32_t)(now_ms - state->first_tap_ms) >= 80u && //This is after the trheshold.
            (uint32_t)(now_ms - state->first_tap_ms) < 400u) {
        state->pending = false;
        return TAP_DOUBLE;
    }
    return TAP_NONE;
}


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
