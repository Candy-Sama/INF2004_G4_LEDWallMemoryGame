#ifndef imu_orientation_h
#define imu_orientation_h

// Panel orientation from the accelerometer's gravity vector.
// Header-only and free of Pico/RTOS calls so it can be host-tested, like
// motion_detector.h.
//
// Assumed sensor axes (check against your mounting, see ORIENT_SENSOR_OFFSET):
//   +X = towards the panel's RIGHT edge, +Y = towards the panel's TOP edge,
//   +Z = out of the LED face. An accelerometer reads +1 g along whichever
//   axis currently points UP, so a panel hanging upright reads Y ~ +1000 mg.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "node_config.h"

typedef enum {
    ORIENT_UNKNOWN = 0,
    ORIENT_UPRIGHT,       // normal wall mounting, top edge up
    ORIENT_ROT_CW90,      // panel turned clockwise 90 degrees (old left edge on top)
    ORIENT_UPSIDE_DOWN,   // turned 180 degrees
    ORIENT_ROT_CCW90,     // turned anticlockwise 90 degrees (old right edge on top)
    ORIENT_FACE_UP,       // lying flat, LEDs facing the ceiling
    ORIENT_FACE_DOWN      // lying flat, LEDs facing the table
} orientation_t;

typedef struct {
    orientation_t stable;        // last confirmed orientation
    orientation_t pending;       // candidate waiting to be confirmed
    uint32_t pending_since_ms;
    uint8_t image_turns;         // clockwise quarter turns to apply to images
} orientation_detector_t;

static inline int orient_abs(int v) { return v < 0 ? -v : v; }

static inline const char *orientation_name(orientation_t o) {
    switch (o) {
        case ORIENT_UPRIGHT:     return "upright";
        case ORIENT_ROT_CW90:    return "rotated CW 90";
        case ORIENT_UPSIDE_DOWN: return "upside down";
        case ORIENT_ROT_CCW90:   return "rotated CCW 90";
        case ORIENT_FACE_UP:     return "face up (flat)";
        case ORIENT_FACE_DOWN:   return "face down (flat)";
        default:                 return "unknown";
    }
}

static inline bool orientation_is_flat(orientation_t o) {
    return o == ORIENT_FACE_UP || o == ORIENT_FACE_DOWN;
}

static inline bool orientation_is_upright_family(orientation_t o) {
    return o >= ORIENT_UPRIGHT && o <= ORIENT_ROT_CCW90;
}

// Clockwise quarter turns the panel itself is rotated (0..3), or -1 if flat/unknown.
static inline int orientation_panel_turns(orientation_t o) {
    switch (o) {
        case ORIENT_UPRIGHT:     return 0;
        case ORIENT_ROT_CW90:    return 1;
        case ORIENT_UPSIDE_DOWN: return 2;
        case ORIENT_ROT_CCW90:   return 3;
        default:                 return -1;
    }
}

// One raw sample -> the orientation it suggests, or `hold` when the sample is
// ambiguous (moving, near a boundary) and should not change anything.
static inline orientation_t orientation_classify(int x, int y, int z, orientation_t hold) {
    // Reject samples taken while the board is being moved: magnitude far from 1 g.
    long mag2 = (long)x * x + (long)y * y + (long)z * z;
    if (mag2 < (long)ORIENT_MIN_MG * ORIENT_MIN_MG ||
        mag2 > (long)ORIENT_MAX_MG * ORIENT_MAX_MG) {
        return hold;
    }
    int ax = orient_abs(x), ay = orient_abs(y), az = orient_abs(z);

    if (az >= ORIENT_FLAT_ENTER_MG) {
        return z > 0 ? ORIENT_FACE_UP : ORIENT_FACE_DOWN;
    }
    // Hysteresis: once flat, stay flat until clearly tilted away.
    if (orientation_is_flat(hold) && az > ORIENT_FLAT_EXIT_MG) {
        return hold;
    }
    if (ax < ORIENT_INPLANE_MIN_MG && ay < ORIENT_INPLANE_MIN_MG) {
        return hold;   // nearly flat but not flat enough: ambiguous
    }

    // Hysteresis between the two in-plane axes (avoids flicker at 45 degrees).
    bool use_y;
    if (hold == ORIENT_UPRIGHT || hold == ORIENT_UPSIDE_DOWN) {
        use_y = !(ax > ay + ORIENT_AXIS_HYST_MG);
    } else if (hold == ORIENT_ROT_CW90 || hold == ORIENT_ROT_CCW90) {
        use_y = ay > ax + ORIENT_AXIS_HYST_MG;
    } else {
        use_y = ay >= ax;
    }
    if (use_y) return y > 0 ? ORIENT_UPRIGHT : ORIENT_UPSIDE_DOWN;
    return x > 0 ? ORIENT_ROT_CCW90 : ORIENT_ROT_CW90;
}

// Image rotation (clockwise quarter turns) that keeps content upright.
static inline uint8_t orientation_image_turns(orientation_t o, uint8_t previous) {
    int k = orientation_panel_turns(o);
    if (k < 0) return previous;   // flat/unknown: keep the last upright rotation
    k = (k + ORIENT_SENSOR_OFFSET) & 3;
    return (uint8_t)((4 - k) & 3);
}

static inline void orientation_detector_init(orientation_detector_t *d) {
    d->stable = ORIENT_UNKNOWN;
    d->pending = ORIENT_UNKNOWN;
    d->pending_since_ms = 0;
    d->image_turns = 0;
}

// Feed every new sample. Returns true only when the confirmed orientation
// changes (after it has been stable for ORIENT_SETTLE_MS).
static inline bool orientation_update(orientation_detector_t *d, int x, int y, int z,
                                      uint32_t now_ms) {
    orientation_t candidate = orientation_classify(x, y, z, d->stable);
    if (candidate == d->stable) {
        d->pending = d->stable;         // nothing waiting to be confirmed
        return false;
    }
    if (candidate != d->pending) {
        d->pending = candidate;
        d->pending_since_ms = now_ms;
        return false;
    }
    if ((uint32_t)(now_ms - d->pending_since_ms) < ORIENT_SETTLE_MS) {
        return false;
    }
    d->stable = candidate;
    d->image_turns = orientation_image_turns(candidate, d->image_turns);
    return true;
}

// Wire format for the shared rotation: exactly one byte, 0..3 (clockwise quarter
// turns). Anything else is rejected so a stray message cannot garble the wall.
static inline bool orientation_sync_decode(const uint8_t *payload, size_t length,
                                           uint8_t *image_turns) {
    if (payload == NULL || image_turns == NULL || length != 1 || payload[0] > 3) {
        return false;
    }
    *image_turns = payload[0];
    return true;
}

#endif