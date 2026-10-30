# MQTT Tribunal 1 review and learning plan

Reviewed on 22 October 2026 against newMain commit 1bf51bf.
This review records implementation evidence and remaining MQTT work. It does
not change runtime behaviour or claim hardware recovery has been verified.

Sources: the supplied Change Requirement 1 and Tribunal 1 Word documents;
current CMake, FreeRTOS configuration, main.c, network module and protocol docs.
The robot-car examples in the change brief are generic examples; the applicable
requirement for this project is compact application data, including task queue
messages and MQTT payloads, safe shared access, and measured evidence.

## What FreeRTOS changed

The old main loop has been divided into independently scheduled tasks. main()
creates queues/tasks and starts the scheduler. Network initialisation now runs
inside net_task after the scheduler starts. Do not move it before the scheduler.

| Task | Priority | Stack in 32-bit words | Responsibility |
| --- | --- | --- | --- |
| sensor_task | 3 | 1024 (4096 bytes) | Read sensor and queue movement events |
| net_task | 2 | 2048 (8192 bytes) | Own Wi-Fi/MQTT, drain events, route display commands |
| display_task | 2 | 512 (2048 bytes) | Own PIO/LED rendering, wait on display queue |

The SDK also creates Wi-Fi/lwIP tasks. Current settings put the Wi-Fi context at
priority 4 and tcpip at 5. Higher task priority means more urgent, not a guarantee
of latency. Stack sufficiency and timing need measurements on hardware.

net_event_q has eight entries; display_q has four. The transport has a separate
eight-message receive FIFO protected by the lwIP lock; it is not a FreeRTOS queue.
That FIFO can remain with one network owner. It is not automatically wrong just
because the application now uses FreeRTOS.

Use vTaskDelay(pdMS_TO_TICKS(ms)) for task waiting. Queue sends copy the queue item
by value. A queued pointer only copies the pointer: its target must remain valid.
Only net_task calls public MQTT APIs. SDK callbacks assemble messages/update
transport state; they must not wait on application work or render LEDs.

## Requirements versus current implementation

| Requirement | Status | Code evidence / remaining work |
| --- | --- | --- |
| FreeRTOS network owner | Present, hardware validation pending | main.c has net_task; CMake links sys_freertos and kernel; NO_SYS=0 |
| Local broker | Firmware configured | network_config.h points at 172.20.10.3; running broker, IP stability, firewall and LAN access require deployment verification |
| MQTT reconnect | Present | mqtt_handler.c retries failed attempts at minimum five-second intervals |
| Restore subscriptions | Present | Connection callback clears active flags; saved filters are subscribed again |
| Initial Wi-Fi connection retries | Present | net_task startup loop retries after five seconds |
| Wi-Fi rejoin after an established link drops | Missing explicit recovery | Runtime loop only logs and waits; it does not call a rejoin procedure |
| Last Will / retained presence | Present as text | offline Will, 10-second keepalive; online status uses QoS 1 and retain |
| Compact binary application messages | Missing in running application | Heart, Dark Heart, OFF, online and offline are still strings |
| Compact binary Last Will | API gap | mqtt_configure_will copies text using strlen; add explicit payload length and populate SDK will_msg_len before using binary zero bytes |
| Packed input queue | Missing | app_event_t enum carries only a motion event; no packed node/tap/fault contract |
| Retained node-to-position mapping | Transport capable; application missing | mqtt_send_ex supports retain; no assignment publisher/receiver or agreed binary format |
| Detect which required board disconnected | Missing application routing | Nodes publish presence, but no master status subscriptions/availability tracking |
| Restore game state before enabling inputs | Missing | mqtt_is_ready means connected and subscribed, not synchronised with master |
| Shared packed state safety | Not integrated | Current game-engine C files are empty; use a game-event queue/single game owner, or agree mutex boundaries if sharing |
| RAM/message-size/timing evidence | Missing | Requires a representation table and before/after measurements |

Queue and MQTT subscription/publish failures must be handled. Sensor and display
xQueueSend return values are currently ignored, so those queues can silently
lose events. MQTT FIFO losses are counted, but do not yet force application resync.

## Reconnection is three separate steps

1. Restore Wi-Fi and obtain an IP address.
2. Restore MQTT and all subscriptions (already largely implemented).
3. Obtain current assignment/game state, have application/display apply it, and
   acknowledge that state before the game owner resumes inputs.

A Last Will informs other clients after the broker detects an unexpected loss.
It does not reconnect a board, guarantee instant detection, or restore game state.
A broker PUBACK means broker acceptance, not that the display applied a command.

Recommended network-owner state sequence:

```
WIFI_CONNECTING -> MQTT_CONNECTING -> SUBSCRIBING -> SYNCING -> READY
       ^                 ^                              |
       |                 |                              |
       +-- Wi-Fi loss ---+------ MQTT loss --------------+
```

Conceptual task loop (design guidance, not implemented):

```
if Wi-Fi unavailable:
    close/reset old MQTT connection safely once, invalidate sync readiness
    discard stale input events and notify the application of network loss
    start/retry Wi-Fi join on a timed schedule
else:
    keep calling init_mqtt_client to service broker/subscription retries
    after mqtt_is_ready becomes true:
        publish presence and request current assignment/state
        stay SYNCING until application/display confirms the state was applied
    dispatch valid incoming messages through bounded application queues
    accept physical input for transmission only while application-ready
wait briefly using vTaskDelay
```

Add explicit transport reset/disconnect handling when recovering Wi-Fi. Ensure
pending DNS/connection callbacks cannot revive an obsolete attempt. Do not just
call cyw43_arch_init repeatedly, create a new mqtt_client on every retry, or
replay the old input queue after reconnecting. Keep MQTT servicing periodic and
avoid draining an unbounded producer queue indefinitely.

For a first implementation, timed blocking Wi-Fi attempts in net_task are easy
to follow; other tasks can run while the SDK waits. That task cannot service its
application queues during the attempt, however. Use short bounded attempts or
an asynchronous join/state machine when timing/queue requirements demand it.

Your module reports NETWORK_LOST / SYNC_REQUIRED / NETWORK_READY. The game owner
preserves and pauses the game, and decides when all required nodes are ready.
The layout owner supplies identity/assignment; networking carries the data.

## Compact data implementation order

1. Agree valid ranges and exact bit positions with input/game/layout owners.
2. Define shared encode/decode functions using uint8_t/uint16_t and masks/shifts.
   Do not transmit a raw C bit-field struct: layout/padding is compiler-dependent.
3. Validate payload length, field ranges and unused bits before dispatching.
4. Publish binary buffers using explicit lengths; never strlen(binary_payload).
5. For critical events use agreed QoS 1, no retain for taps, and retained latest
   state/assignment as appropriate. QoS 1 duplicates require agreed event IDs and
   duplicate handling; don't promise exactly-once gameplay from broker ACKs.
6. Update the shared protocol document and compare bytes/RAM before and after.

Example candidate only: six predefined IDs require three bits; tap and IMU fault
need one bit each. Those five meaningful bits fit one byte. Confirm that the input
owner actually needs these fields on the wire before standardising the format.
Likewise 0..5 positions require three bits. Protocol version, session/revision
ranges and deduplication requirements must be agreed rather than guessed.

## Important integration corrections

- apply_received_command handles OFF by calling ledmatrix_fill in net_task.
  Route it through show_colour/display_q so display_task owns all LED writes.
- Queue overflow must be visible and have a policy. Use the return value of
  xQueueSend; state loss should notify the game owner/resync rather than silently
  claim success.
- handle_sensor_events runs before MQTT readiness and events may accumulate
  during the long startup join. Gate transmission on application-ready and
  discard pre-ready/stale physical inputs. Transport readiness alone is weaker
  than game synchronisation readiness.
- Check xTaskCreate results and measure stack high-water marks under load.
- docs/MQTT_PROTOCOL.md still proposes JSON and claims no FreeRTOS exists.
  README.md also describes the old main-loop architecture. These are stale;
  update them with the agreed binary protocol after the field contract is settled.
- RTOS_SMOKETEST selects src/main_rtos_smoketest.c, but the existing alternative
  source is named main_rtos_smokeclean.c. Normal build uses main.c and succeeds;
  the optional smoke-test selection needs a separate correction.

## What to implement first

First add/test runtime Wi-Fi recovery in the owning net_task, keeping the current
message format temporarily. Verify broker/subscription restoration independently.
Then implement the agreed compact protocol and game resync interface. This keeps
failures attributable to one change at a time; it does not make text messages
compliant with the final minimum-bit requirement.

## Verification completed during this review

- Clean normal FreeRTOS build succeeded in build/tribunal-audit (217 steps).
- Existing MQTT host tests passed, including reconnect and resubscription,
  Last Will configuration, QoS/retain options and callback forwarding.
- Existing Wi-Fi parameter/retry host tests passed.
- No firmware source or teammate modules changed during this review.

Those host tests mock the SDK; they do not verify FreeRTOS scheduling, real
network recovery, broker Last Will timing or physical display latency.

Hardware evidence to collect:

| Test | Expected evidence |
| --- | --- |
| Stop/restart broker while Wi-Fi stays on | Disconnect log, retry, resubscriptions, fresh presence |
| Turn AP/hotspot off then on without resetting Pico | Wi-Fi rejoin, MQTT rejoin, input disabled until resync |
| Power off a required slave mid-game | Broker Will/master loss event and game-owner pause |
| Reconnect slave during reveal/mismatch | Current state restored; no old input replay; applied ACK before resume |
| Inject duplicate QoS 1 selections | One game action per agreed event ID |
| Burst input / fill display and MQTT queues | Explicit drop/backpressure/resync logs, no silent corruption |
| Render while publishing from two nodes | End-to-end timestamps and worst-case latency, queue/stack metrics |
| Send invalid binary values/lengths | Rejection without game-state mutation |

Use the agreed target for nonnegative elapsed latency (for example <=200 ms),
not the imprecise +/-0.2-second phrasing in some documents. Compare physical
input-to-display latency, not just broker round-trip time.
