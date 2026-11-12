#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "node_config.h"
#include "node_identity.h"

/* Central-PC -> nodes "play animation X at time T" plus the shared clock that
 * makes T mean the same instant on every node. Protocol: docs/MQTT_PROTOCOL.md s9. */

#ifndef SYNC_MASTER_NODE
#define SYNC_MASTER_NODE      4          /* node that publishes ticks and stamps start times. Must be a node that
                                            actually exists: with boards 4 and 5 only, set 4 (e.g. -DSYNC_MASTER_NODE=4) */
#endif
#define IS_SYNC_MASTER        (identity_node_id() == SYNC_MASTER_NODE)

#define MQTT_SYNC_FILTER      MQTT_TOPIC_ROOT "/sync/+"      /* ONE subscription slot for all three */
#define MQTT_SYNC_TICK_TOPIC  MQTT_TOPIC_ROOT "/sync/tick"   /* master -> all, QoS 0, not retained  */
#define MQTT_SYNC_REQ_TOPIC   MQTT_TOPIC_ROOT "/sync/req"    /* PC -> master                        */
#define MQTT_SYNC_PLAY_TOPIC  MQTT_TOPIC_ROOT "/sync/play"   /* master -> all, QoS 1, not retained  */

#define SYNC_MIN_LEAD_MS      150        /* shortest delay the master accepts (all nodes must hear it first) */
#define SYNC_LATE_TOL_US      20000      /* started this long ago or less: still play; older: skip + log */
#define SYNC_MAX_LEAD_US      65000000   /* refuse a start further away than this */
#define SYNC_SPIN_US          1500       /* sleep until this close, then busy-wait */
#define SYNC_LOG_TICKS        1          /* 1 = print every tick (evidence runs), 0 = quiet */

#ifndef SYNC_PULSE_GPIO
#define SYNC_PULSE_GPIO       22         /* 5 ms pulse at fire time for a logic analyser; -1 = off.
                                            Confirm the pin is free on your board (buzzer GP18, LED GP2, IMU GP0/1). */
#endif

bool     clock_sync_start(void);         /* call before net_start(); registers its own net handler */
void     clock_sync_poll(void);          /* net_task only: the clock master publishes its tick here */
bool     clock_sync_is_synced(void);

/* Weak hook. Another module (the animation owner) overrides it by defining a normal
 * function with this name. anim_id 0..3 = A..D; slot = this node's grid position
 * (node id - 1), so a long image can pick its own slice. Default: flash a colour. */
void     anim_on_play(uint8_t anim_id, uint8_t slot);