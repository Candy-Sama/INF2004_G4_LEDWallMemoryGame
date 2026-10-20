#include "network_recovery.h"

static uint64_t read_token(const uint8_t *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}
static uint32_t read_revision(const uint8_t *p) {
    return ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) |
           ((uint32_t)p[10] << 8) | p[11];
}
static bool matches(const network_recovery_t *r, const void *data, size_t length) {
    const uint8_t *p = data;
    return p && length >= RECOVERY_HEADER_SIZE && read_token(p) == r->token;
}
void recovery_init(network_recovery_t *r, uint64_t nonce) {
    *r = (network_recovery_t){.token = nonce, .phase = RECOVERY_OFFLINE};
}
void recovery_begin(network_recovery_t *r, uint32_t now) {
    // New token rejects delayed messages from any previous connection/retry.
    // Refuse wrap instead of reusing a token. Boot nonce must change on reboot.
    if (r->token == UINT64_MAX) { r->phase = RECOVERY_FAILED; return; }
    ++r->token;
    r->revision = 0;
    r->phase = RECOVERY_REQUESTING;
    r->phase_started_ms = now;
    r->sent = false;
}
void recovery_disconnect(network_recovery_t *r) { r->phase = RECOVERY_OFFLINE; }
bool recovery_state(network_recovery_t *r, const void *data, size_t length, uint32_t now) {
    if (!matches(r, data, length) || length == RECOVERY_HEADER_SIZE ||
        r->phase == RECOVERY_OFFLINE || r->phase == RECOVERY_FAILED) return false;
    uint32_t rev = read_revision(data);
    if (r->phase != RECOVERY_REQUESTING && rev <= r->revision) return false;
    r->revision = rev;
    r->phase = RECOVERY_APPLYING;
    r->phase_started_ms = now;
    r->sent = false;
    return true;
}
bool recovery_applied(network_recovery_t *r, const void *data, size_t length, uint32_t now) {
    if (length != RECOVERY_HEADER_SIZE || !matches(r, data, length) ||
        read_revision(data) != r->revision || r->phase != RECOVERY_APPLYING) return false;
    r->phase = RECOVERY_WAITING_RESUME;
    r->phase_started_ms = now;
    r->sent = false;
    return true;
}
bool recovery_resume(network_recovery_t *r, const void *data, size_t length, uint32_t now) {
    if (length != RECOVERY_HEADER_SIZE || !matches(r, data, length) ||
        read_revision(data) != r->revision ||
        (r->phase != RECOVERY_WAITING_RESUME && r->phase != RECOVERY_READY)) return false;
    r->phase = RECOVERY_READY;
    r->last_resume_ms = now;
    return true;
}
bool recovery_tick(network_recovery_t *r, uint32_t now) {
    if ((r->phase == RECOVERY_READY && (uint32_t)(now-r->last_resume_ms) >= RECOVERY_LEASE_MS) ||
        (r->phase == RECOVERY_APPLYING && (uint32_t)(now-r->phase_started_ms) >= RECOVERY_LEASE_MS)) {
        recovery_begin(r, now);
        return true;
    }
    return false;
}
size_t recovery_outgoing(network_recovery_t *r, uint32_t now, uint8_t p[12]) {
    if (r->phase != RECOVERY_REQUESTING && r->phase != RECOVERY_WAITING_RESUME) return 0;
    if (r->sent && (uint32_t)(now-r->last_send_ms) < RECOVERY_RETRY_MS) return 0;
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(r->token >> (56-8*i));
    for (unsigned i = 0; i < 4; ++i) p[8+i] = (uint8_t)(r->revision >> (24-8*i));
    r->last_send_ms = now;
    r->sent = true;
    return r->phase == RECOVERY_REQUESTING ? 8 : 12;
}
