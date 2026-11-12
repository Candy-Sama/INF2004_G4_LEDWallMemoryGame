#ifndef WIRE_FORMAT_H
#define WIRE_FORMAT_H

/*
 * Compact binary MQTT payloads (Change Requirement: minimum-bit data).
 *
 * Header-only and free of Pico/RTOS calls so it can be host-tested.
 * Every payload is packed with explicit shifts and masks, NOT a bit-field struct
 * cast onto the buffer: C leaves bit-field layout to the compiler, so a struct is
 * fine inside RAM but is not a reliable wire format.
 *
 * Bit 7 is the most significant bit of each byte.
 *
 *  Payload                 Topic suffix      Bytes  Layout
 *  ----------------------  ----------------  -----  ------------------------------------
 *  Display command         nodeN/command     1      cmd[7:5] arg[4:0]
 *  Presence + faults       nodeN/status      1      pres[7:6] faults[5:0]
 *  Tap event               game/tap          1      node[7:5] seq[4:1] spare[0]
 *  Setup message           game/setup        1      op[7] node[6:4] spare[3:0]
 *  Join request            join              8      raw 64-bit Pico unique board ID
 *  Join reply              assign/<board>    1      node[2:0]
 *  Orientation (existing)  orientation       1      turns[1:0]
 *  Clock tick              sync/tick         5      seq[7:4] spare[3:0] | master_us[31:0]
 *  Play request (PC)       sync/req          3      anim[7:6] spare[5:0] | delay_ms[15:0]
 *  Play command            sync/play         5      anim[7:6] seq[5:2] spare[1:0] | start_us[31:0]
 *

 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------ display command */

typedef enum {
    WIRE_CMD_OFF  = 0,   /* arg unused: panel dark / face down */
    WIRE_CMD_CARD = 1,   /* arg = pair id 0..2 */
    WIRE_CMD_COUNT       /* must stay <= 8 (3 bits) */
} wire_cmd_t;

#define WIRE_PAIR_MAX 2   /* pair ids are 0..2 (3 pairs) */

static inline uint8_t wire_cmd_encode(wire_cmd_t cmd, uint8_t arg) {
    return (uint8_t)(((unsigned)cmd & 0x7u) << 5 | (arg & 0x1Fu));
}

static inline bool wire_cmd_decode(const uint8_t *p, size_t len,
                                   wire_cmd_t *cmd, uint8_t *arg) {
    if (p == NULL || len != 1 || cmd == NULL || arg == NULL) return false;
    unsigned c = p[0] >> 5;
    uint8_t  a = p[0] & 0x1Fu;
    if (c >= WIRE_CMD_COUNT) return false;
    if (c == WIRE_CMD_CARD && a > WIRE_PAIR_MAX) return false;
    *cmd = (wire_cmd_t)c;
    *arg = a;
    return true;
}

/* ------------------------------------------------ presence + fault flags */

/* Presence code in bits 7:6. 0 and 3 are invalid. The Last Will is 0x40, which is
 * non-zero on purpose: mqtt_configure_will() takes a C string, and a 0x00 byte
 * would be read as an empty payload. */
#define WIRE_PRES_OFFLINE 1u
#define WIRE_PRES_ONLINE  2u

/* Shared fault bitmask, bits 5:0. One bit per fault, never separate variables. */
#define FAULT_IMU            (1u << 0)   /* IMU read error / not found       */
#define FAULT_WIFI_DROPPED   (1u << 1)   /* Wi-Fi link was lost              */
#define FAULT_MQTT_DROPPED   (1u << 2)   /* broker session was lost          */
#define FAULT_RX_OVERFLOW    (1u << 3)   /* received messages were discarded */
#define FAULT_PUBLISH_FAILED (1u << 4)   /* an outgoing publish was refused  */
#define FAULT_DISPLAY        (1u << 5)   /* display command queue was full   */
#define FAULT_MASK           0x3Fu

#define WIRE_STATUS_OFFLINE_BYTE ((uint8_t)(WIRE_PRES_OFFLINE << 6))
#define WIRE_STATUS_OFFLINE_STR  "\x40"   /* same byte as a C string, for the Will */

static inline uint8_t wire_status_encode(bool online, uint8_t faults) {
    uint8_t pres = online ? WIRE_PRES_ONLINE : WIRE_PRES_OFFLINE;
    return (uint8_t)(pres << 6 | (faults & FAULT_MASK));
}

/* true = valid. online and faults are filled in. */
static inline bool wire_status_decode(const uint8_t *p, size_t len,
                                      bool *online, uint8_t *faults) {
    if (p == NULL || len != 1 || online == NULL || faults == NULL) return false;
    unsigned pres = p[0] >> 6;
    if (pres != WIRE_PRES_OFFLINE && pres != WIRE_PRES_ONLINE) return false;
    *online = (pres == WIRE_PRES_ONLINE);
    *faults = p[0] & FAULT_MASK;
    return true;
}

/* ------------------------------------------------------------- tap event */

#define WIRE_NODE_MAX 6   /* node ids are 1..6 */

static inline uint8_t wire_tap_encode(uint8_t node, uint8_t seq) {
    return (uint8_t)((node & 0x7u) << 5 | (seq & 0xFu) << 1);
}

static inline bool wire_tap_decode(const uint8_t *p, size_t len,
                                   uint8_t *node, uint8_t *seq) {
    if (p == NULL || len != 1 || node == NULL || seq == NULL) return false;
    uint8_t n = p[0] >> 5;
    if (n < 1 || n > WIRE_NODE_MAX) return false;
    *node = n;
    *seq  = (p[0] >> 1) & 0xFu;
    return true;
}

/* ----------------------------------------------------------- setup message */

#define WIRE_SETUP_WRONG 0u   /* node = the panel that is in the wrong place */
#define WIRE_SETUP_START 1u   /* node unused */

static inline uint8_t wire_setup_encode(uint8_t op, uint8_t node) {
    return (uint8_t)((op & 0x1u) << 7 | (node & 0x7u) << 4);
}

static inline bool wire_setup_decode(const uint8_t *p, size_t len,
                                     uint8_t *op, uint8_t *node) {
    if (p == NULL || len != 1 || op == NULL || node == NULL) return false;
    uint8_t o = p[0] >> 7, n = (p[0] >> 4) & 0x7u;
    if (o == WIRE_SETUP_WRONG && (n < 1 || n > WIRE_NODE_MAX)) return false;
    *op = o;
    *node = n;
    return true;
}

/* ------------------------------------------------------ join / assignment */

#define WIRE_BOARD_ID_BYTES 8   /* pico_get_unique_board_id: 64 bits */

static inline int wire_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 16 hex characters -> 8 bytes. false if the text is not exactly 16 hex digits. */
static inline bool wire_board_id_pack(const char *hex, uint8_t out[WIRE_BOARD_ID_BYTES]) {
    if (hex == NULL || out == NULL) return false;
    for (size_t i = 0; i < WIRE_BOARD_ID_BYTES; ++i) {
        int hi = wire_hex_nibble(hex[2 * i]);
        int lo = hi < 0 ? -1 : wire_hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return hex[2 * WIRE_BOARD_ID_BYTES] == '\0';
}

/* 8 bytes -> 16 upper-case hex characters plus terminator (out needs 17 bytes). */
static inline void wire_board_id_unpack(const uint8_t in[WIRE_BOARD_ID_BYTES], char out[17]) {
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < WIRE_BOARD_ID_BYTES; ++i) {
        out[2 * i]     = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0xF];
    }
    out[16] = '\0';
}

static inline bool wire_assign_decode(const uint8_t *p, size_t len, uint8_t *node) {
    if (p == NULL || len != 1 || node == NULL) return false;
    uint8_t n = p[0] & 0x7u;
    if (p[0] > 7 || n < 1 || n > WIRE_NODE_MAX) return false;
    *node = n;
    return true;
}

/* ------------------------------------------------ shared clock + animation */
 
#define WIRE_ANIM_COUNT   4        /* animation IDs 0..3 = A, B, C, D */
#define WIRE_TICK_LEN     5
#define WIRE_REQ_LEN      3
#define WIRE_PLAY_LEN     5
#define WIRE_REQ_DELAY_MAX_MS 60000u
 
static inline void wire_put_u32(uint8_t *out, uint32_t v) {
    out[0] = (uint8_t)(v >> 24); out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);  out[3] = (uint8_t)v;
}
static inline uint32_t wire_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
 
/* sync/tick: clock master -> everyone, QoS 0, NEVER retained (a retained tick is
 * a stale timestamp for every late subscriber). master_us = master's time_us_32()
 * taken immediately before the publish call. seq is a 4-bit loss counter. */
static inline void wire_tick_encode(uint8_t seq, uint32_t master_us, uint8_t out[WIRE_TICK_LEN]) {
    out[0] = (uint8_t)((seq & 0xFu) << 4);
    wire_put_u32(out + 1, master_us);
}
static inline bool wire_tick_decode(const uint8_t *p, size_t len, uint8_t *seq, uint32_t *master_us) {
    if (p == NULL || len != WIRE_TICK_LEN || seq == NULL || master_us == NULL) return false;
    if (p[0] & 0x0Fu) return false;                 /* spare bits must be 0 */
    *seq = p[0] >> 4;
    *master_us = wire_get_u32(p + 1);
    return true;
}
 
/* sync/req: central PC -> clock master. "Play animation X, delay_ms from now".
 * The PC cannot know the Picos' clock, so the master converts the relative delay
 * into an absolute start time on the shared timeline. */
static inline void wire_req_encode(uint8_t anim, uint16_t delay_ms, uint8_t out[WIRE_REQ_LEN]) {
    out[0] = (uint8_t)((anim & 0x3u) << 6);
    out[1] = (uint8_t)(delay_ms >> 8);
    out[2] = (uint8_t)delay_ms;
}
static inline bool wire_req_decode(const uint8_t *p, size_t len, uint8_t *anim, uint16_t *delay_ms) {
    if (p == NULL || len != WIRE_REQ_LEN || anim == NULL || delay_ms == NULL) return false;
    if (p[0] & 0x3Fu) return false;
    *anim = p[0] >> 6;
    *delay_ms = (uint16_t)(p[1] << 8 | p[2]);
    return true;
}
 
/* sync/play: clock master -> every node. QoS 1, NEVER retained (a retained play
 * would replay an old animation on reconnect). start_us is on the master timeline
 * (the same clock as the tick), so each node converts it with its own offset. */
static inline void wire_play_encode(uint8_t anim, uint8_t seq, uint32_t start_us, uint8_t out[WIRE_PLAY_LEN]) {
    out[0] = (uint8_t)((anim & 0x3u) << 6 | (seq & 0xFu) << 2);
    wire_put_u32(out + 1, start_us);
}
static inline bool wire_play_decode(const uint8_t *p, size_t len, uint8_t *anim, uint8_t *seq, uint32_t *start_us) {
    if (p == NULL || len != WIRE_PLAY_LEN || anim == NULL || seq == NULL || start_us == NULL) return false;
    if (p[0] & 0x03u) return false;
    *anim = p[0] >> 6;
    *seq = (p[0] >> 2) & 0xFu;
    *start_us = wire_get_u32(p + 1);
    return true;
}


#endif /* WIRE_FORMAT_H */
