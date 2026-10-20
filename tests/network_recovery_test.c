#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "network_recovery.h"
int main(void) {
    network_recovery_t r;
    uint8_t header[12], state[13], old[12];
    recovery_init(&r, 42);
    assert(recovery_outgoing(&r, 0, header) == 0);
    recovery_begin(&r, 0);
    assert(recovery_outgoing(&r, 0, header) == 8 && header[7] == 43);
    assert(recovery_outgoing(&r, 199, header) == 0);
    assert(recovery_outgoing(&r, 200, header) == 8);
    header[11] = 7; // Controller revision 7.
    memcpy(state, header, 12); state[12] = 2;
    assert(!recovery_resume(&r, header, 12, 201)); // Cannot skip state application.
    assert(!recovery_state(&r, state, 12, 201)); // No state body.
    state[7] = 42; assert(!recovery_state(&r, state, 13, 201));
    state[7] = 43; assert(recovery_state(&r, state, 13, 201));
    assert(!recovery_state(&r, state, 13, 202)); // Duplicate does not render twice.
    assert(!recovery_resume(&r, header, 12, 202));
    header[11] = 6; assert(!recovery_applied(&r, header, 12, 202));
    header[11] = 7; assert(recovery_applied(&r, header, 12, 202));
    assert(recovery_outgoing(&r, 202, header) == 12);
    assert(recovery_outgoing(&r, 401, header) == 0);
    assert(recovery_outgoing(&r, 402, header) == 12); // Lost applied/resume retries.
    assert(recovery_resume(&r, header, 12, 403) && r.phase == RECOVERY_READY);
    assert(recovery_resume(&r, header, 12, 1403)); // Refresh controller lease.
    assert(!recovery_tick(&r, 4402));
    memcpy(old, header, 12);
    assert(recovery_tick(&r, 4403) && r.phase == RECOVERY_REQUESTING);
    assert(!recovery_resume(&r, old, 12, 4404)); // Old session cannot resume.
    assert(!recovery_applied(&r, old, 12, 4404));
    recovery_disconnect(&r);
    assert(r.phase == RECOVERY_OFFLINE && !recovery_state(&r, state, 13, 4405));
    recovery_begin(&r, UINT32_MAX-100);
    assert(recovery_outgoing(&r, UINT32_MAX-100, header) == 8);
    assert(recovery_outgoing(&r, 100, header) == 8); // Clock wrap safe.
    memcpy(state, header, 12); state[12] = 0;
    assert(recovery_state(&r, state, 13, 101));
    assert(recovery_tick(&r, 3101)); // App never confirms: retry new session.
    r.token = UINT64_MAX; recovery_begin(&r, 0);
    assert(r.phase == RECOVERY_FAILED);
    puts("Recovery handshake tests passed.");
}
