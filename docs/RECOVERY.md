# Connection recovery: what is implemented

## The distinction to remember

`network_ready()` = messages can travel. `network_game_ready()` = this panel
installed a snapshot and the controller authorised play. Recovery-enabled panels
cannot use `network_publish()` to send gameplay while synchronising. The network
layer still does not interpret game/card data.

```text
Panel connects -> sends SYNC token
Controller sends token + revision + complete snapshot
Panel application installs assignment/state/display
Application calls network_state_applied(&original_message)
Network Task sends APPLIED token + revision, retrying as needed
Controller checks ALL required panels, then sends RESUME token + revision
Panel allows input; controller refreshes RESUME at least once per second
```

A transport PUBACK is not an application acknowledgement. The network layer never
automatically acknowledges a snapshot just because it received the bytes.

## Panel setup and application integration

Set `sync_topic`, `state_topic`, `applied_topic`, `resume_topic` and a fresh nonzero
random `boot_nonce` in `network_task_config_t`. `main.c` demonstrates these fields
with `get_rand_64()`. The state and resume topics are automatically subscribed at
QoS 1. Leave all four NULL for legacy/controller transport-only mode.

One old `subscription` filter is still supported. Add more with a static array:

```c
static const char *const filters[] = {
    "se4/memorygame/+/status",
    "se4/memorygame/+/sync",
    "se4/memorygame/+/applied",
    "se4/memorygame/+/tap"
};
// In the config: .subscriptions = filters, .subscription_count = 4
```

Maximum eight filters total, including the two automatic recovery filters.
A controller in transport-only mode can publish snapshots/resume while its game
is paused; its game engine separately controls whether moves are accepted.

The application must continue calling `network_receive()` while transport-ready,
even when game-ready is false: that is how it receives its recovery snapshot.
Use `network_game_ready()` to gate taps, not to gate the recovery dispatcher.

```c
if (network_receive(&message, pdMS_TO_TICKS(20))) {
    if (!strcmp(message.topic, my_state_topic)) {
        // Check length >= 13 and validate application body first.
        // Bytes 0..11 are the recovery header; application bytes start at 12.
        // Install ALL required state (including position) and finish display work.
        // Only then call network_state_applied(&message).
        // If installation fails, do not acknowledge; recovery remains blocked.
    }
}
```

Keep the original header unchanged. When newer state arrives before an old
application acknowledgement, the old acknowledgement is rejected. If queued
acknowledgement fails or the application does not confirm within 3 seconds,
a new sync attempt follows. No partial snapshot/multi-packet assembly is
implemented: the complete body must fit within 244 bytes.

## Binary wire format

This small envelope supersedes the JSON recovery examples in MQTT_PROTOCOL.md.
Other game messages still need the team's compact binary field agreement.
These are session messages: QoS 1, retain=false. Retained canonical assignments
and game state can be stored separately, but cannot substitute for this handshake.

| Topic suffix on a panel | Payload | Length |
| --- | --- | --- |
| `/sync` | 64-bit token | 8 bytes |
| `/state` | token + 32-bit revision + application snapshot | 13..256 bytes |
| `/applied` | token + revision | 12 bytes |
| `/resume` | token + revision | 12 bytes |

Integers use explicit big-endian byte order. Token is a random boot nonce advanced
for every recovery attempt; it rejects replies from earlier attempts/reboots.
It is correlation data, not authentication. Never reuse it; exhaustion fails
closed. Revision is an unsigned 32-bit controller counter for authoritative state;
start a new game/session before wrap and ensure the current snapshot is sent in
response to each new token. No C struct memory layout is transmitted.

The 64/32-bit ranges are explicit, not a claim that the whole firmware meets the
tribunal minimum-bit requirement. Card fields, internal queue storage, statuses
and RAM/message comparisons still need the separate representation review.

## Retry, failure and controller responsibilities

* Request current state every 200 ms until a matching snapshot arrives.
* Reject malformed, wrong-token and older/equal-revision snapshots. Duplicate
  delivery cannot replay display work; applied messages retry independently.
* Wait for application installation, then retry applied every 200 ms until resume.
* Resume before application confirmation is ignored. Match both token and revision.
* Controller repeats resume once per second while it permits gameplay. Three
  seconds without matching resume stops input and starts a new sync. This bounds
  controller-loss detection independently of the broker's slower Last Will.
* Observed Wi-Fi/MQTT loss clears readiness and pending queues. Every accepted
  MQTT connection has a generation counter so a brief missed outage still starts
  a new recovery attempt. Lower MQTT receive losses/application inbox overflow
  also invalidate recovery state.

The REAL controller/game owner must implement registration, assignment uniqueness,
boot/game/revision validity, required-panel tracking, fault/status handling,
freeze/preserve mismatch timers, new snapshots, and the all-panels-ready resume
barrier. Withhold resume when any required node is offline or unsynchronised.
Send a newer paused snapshot to connected panels for visible pause feedback;
the lease is a fallback if controller communication stops. Pausing on a remote
panel failure is not automatic merely because the local network is connected.
Persistent controller restart recovery is not provided here.

## Existing firmware demo

The default main enables recovery. It now needs a controller response before
sensor movement publishes. It accepts a DEMO snapshot body of one byte: colour
0=off, 1=red, 2=green, 3=pink (a two-bit value; upper bits must be zero). It applies
that colour, then queues applied. This is not a complete memory-game snapshot.
Both masters and four slaves remain intended card panels; game roles are not
chosen by this module.

## Try without a Pico

With your local Mosquitto broker running, start these in separate terminals:

```powershell
python tools/recovery_demo.py controller
python tools/recovery_demo.py panel
```

The tool uses installed Mosquitto CLI executables and only Python's standard
library. Use your working Python executable if `python` is not on PATH.
Default broker is 127.0.0.1:1883, panel is node1. `--node node2` changes that panel;
`--host`, `--port`, `--bin-dir` configure deployment. Stop with Ctrl+C.

This is a ONE-PANEL colour demonstration, not the six-panel game controller.
Stop the controller: after three seconds the simulated panel stops being ready
and requests sync again. Restart controller: snapshot -> applied -> resume runs
again. `--once` exits the simulated panel after its first successful handshake;
`--duration 5` stops either process after five seconds.

For a physical demo Pico, run just the controller against the LAN broker and its
node ID. Configure the broker LAN address and Wi-Fi before building/flashing.
Do not run a pretend panel and physical panel with the same node topic together.

## Verification

Host tests cover retry intervals, malformed/stale/duplicate replies, premature
resume, application timeout, controller lease, clock/token wrap, queue routing,
application acknowledgement and missed-outage connection generations. The task
tests now run one continuous worker loop rather than restarting it each step.
The PC demo was exercised through an isolated local Mosquitto broker. Pico radio,
LED latency, FreeRTOS timing and the complete six-panel game still need hardware
and team integration testing.
