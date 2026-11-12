#include <assert.h>
#include <stdio.h>
#include "motion_detector.h"

int main(void) {
    tap_classifier_t classifier = {0};

    assert(tap_classifier_update(&classifier, true, 0) == TAP_NONE);
    assert(classifier.pending);

    assert(tap_classifier_update(&classifier, false, 399) == TAP_NONE);
    assert(tap_classifier_update(&classifier, false, 400) == TAP_SINGLE);
    assert(!classifier.pending);

    assert(tap_classifier_update(&classifier, false, 500) == TAP_NONE);
    puts("Single-tap test passed.");

    tap_classifier_t double_test = {0};

    assert(tap_classifier_update(&double_test, true, 0) == TAP_NONE);
    assert(tap_classifier_update(&double_test, true, 200) == TAP_DOUBLE);
    assert(!double_test.pending);

    assert(tap_classifier_update(&double_test, false, 400) == TAP_NONE);

    puts("Double-tap test passed.");
   
    tap_classifier_t vibration_test = {0};

    assert(tap_classifier_update(&vibration_test, true, 0) == TAP_NONE);
    assert(tap_classifier_update(&vibration_test, true, 20) == TAP_NONE);
    assert(vibration_test.pending);

    assert(tap_classifier_update(&vibration_test, false, 400) == TAP_SINGLE);
    assert(!vibration_test.pending);

    puts("Early-vibration test passed.");
    
    return 0;
}