#ifndef NETWORK_RECOVERY_H
#define NETWORK_RECOVERY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Recovery envelope, NOT game data. All multi-byte values are big-endian.
// request = token[8]; state = token[8] + revision[4] + application bytes;
// applied/resume = token[8] + revision[4]. Never retain these session messages.
#define RECOVERY_HEADER_SIZE 12
#define RECOVERY_RETRY_MS 200
#define RECOVERY_LEASE_MS 3000

typedef enum {
    RECOVERY_OFFLINE, RECOVERY_REQUESTING, RECOVERY_APPLYING,
    RECOVERY_WAITING_RESUME, RECOVERY_READY, RECOVERY_FAILED
} recovery_phase_t;
typedef struct {
    uint64_t token;
    uint32_t revision, last_send_ms, phase_started_ms, last_resume_ms;
    recovery_phase_t phase;
    bool sent;
} network_recovery_t;

// One owner (Network Task) calls these; they contain no Wi-Fi/RTOS/game logic.
void recovery_init(network_recovery_t *r, uint64_t boot_nonce);
void recovery_begin(network_recovery_t *r, uint32_t now);
void recovery_disconnect(network_recovery_t *r);
// Returns true only for a fresh matching state to hand to the application.
bool recovery_state(network_recovery_t *r, const void *data, size_t length, uint32_t now);
bool recovery_applied(network_recovery_t *r, const void *header, size_t length, uint32_t now);
bool recovery_resume(network_recovery_t *r, const void *header, size_t length, uint32_t now);
// Restart sync after application timeout or controller lease expiry.
bool recovery_tick(network_recovery_t *r, uint32_t now);
// Build the next retry: length 8=request, 12=applied, 0=nothing due.
size_t recovery_outgoing(network_recovery_t *r, uint32_t now, uint8_t header[12]);
#endif
