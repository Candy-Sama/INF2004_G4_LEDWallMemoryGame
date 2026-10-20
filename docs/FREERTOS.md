# FreeRTOS networking starter

**Recovery update:** read [RECOVERY.md](RECOVERY.md). Panels can now opt into
snapshot/application-acknowledgement/controller-resume recovery. The default
demo enables it; `network_game_ready()` gates input. Multiple filters are supported.


The firmware now starts the FreeRTOS scheduler, not a main-loop superloop.
Two application-owned tasks run on core 0. SDK tasks service lwIP and CYW43;
"one network owner" means one caller of our Wi-Fi/MQTT APIs, not one task inside
the complete networking stack.

```text
Application Task -- network_publish() --> TX queue --> Network Task --> MQTT
Application Task <-- network_receive() -- RX queue <-- Network Task <-- MQTT
```

`main()` creates queues and tasks, then calls `vTaskStartScheduler()`.
Network Task initialises Wi-Fi after the scheduler starts, configures the Will
and command subscription, services MQTT, publishes demo presence, and transfers
bytes between queues. Application Task owns IMU/LED hardware and interprets the
demo RED/GREEN/OFF messages. Neither MQTT nor Network Task parses game commands.

Both loops block for 10 ms using `vTaskDelay()`, allowing other ready tasks to
run. Network priority is 3, application priority 2, SDK async/timer priority 4,
lwIP TCP/IP priority 5. These are initial settings, not measured deadlines.
Sensor/display are still combined in the example Application Task; teammates
can split them later. No game task is implemented here.

## Build dependency

FreeRTOS Kernel V11.2.0 is downloaded locally but deliberately not vendored into
this repository. Teammates run this once from the repository root:

```powershell
git clone --depth 1 --branch V11.2.0 https://github.com/FreeRTOS/FreeRTOS-Kernel.git lib/FreeRTOS-Kernel
```

The tested commit is `0adc196d4bd52a2d91102b525b0aafc1e14a2386`.
CMake defaults to that directory; `-DFREERTOS_KERNEL_PATH=...` supports an
existing installation. Build with the existing Pico SDK toolchain and target
`pico_w`. CMake links `FreeRTOS-Kernel-Heap4` and
`pico_cyw43_arch_lwip_sys_freertos`; lwIP uses `NO_SYS=0` and core locking.
FreeRTOS heap is 96 KiB; lwIP heap is 16 KiB. Stack/heap sizes need measurement
on hardware. Allocation and stack-overflow hooks fail visibly instead of hiding
the failure. UART logging remains disabled because GP0/GP1 are used for I2C.

## Calls for teammates

Use `network_task.h` from ordinary tasks. Recovery-enabled panels must use
`network_game_ready()` before accepting taps. Continue receiving snapshots while
`network_ready()` is true, even if game readiness is false. No calls here are ISR-safe.

```c
const char tap[] = "TAP";
bool queued = network_publish(tap_topic, tap, sizeof(tap) - 1, 1, false);
// Arguments: full topic, bytes, length, QoS, retain.
// true means copied into the owner's queue, not delivered/applied remotely.

mqtt_message_t message;
if (network_receive(&message, pdMS_TO_TICKS(20))) {
    // Dispatch by topic. Check message.length; payload is not a C string.
}
```

There must be one application receiver/dispatcher per board; multiple receivers
would steal messages from one another. Route messages onward to dedicated game
and display queues during integration. Do not call `mqtt_send`, `mqtt_receive`,
Wi-Fi APIs or MQTT completion callbacks from other tasks. The original transport
functions remain available inside the Network Task, with the same semantics.

### Understanding `wait_ticks` and `&message`

```c
mqtt_message_t message;
bool received = network_receive(&message, pdMS_TO_TICKS(20));
```

`message` is a variable holding one complete message. `&message` means "the
address of that variable". Passing its address lets `network_receive` fill the
original variable, rather than modifying a temporary copy. Read its topic,
payload and length only when the function returns true.

`wait_ticks` is the maximum time this calling task waits if the queue is empty:

| Argument | Behaviour |
| --- | --- |
| `0` | Check once and return immediately |
| `pdMS_TO_TICKS(20)` | Wait up to about 20 ms; return sooner if a message arrives |
| `portMAX_DELAY` | Wait indefinitely in this configuration |

FreeRTOS measures waiting in ticks. Our 1000 Hz tick is currently 1 ms, but
always use `pdMS_TO_TICKS` to convert milliseconds if the tick rate changes.
Waiting blocks only the caller; the Network Task and other ready tasks continue.
Do not use an indefinite wait in a task that also needs periodic sensor work.

Readiness uses one boolean protected by short critical sections. A separate
event-group allocation is unnecessary because no task waits on readiness bits.
The owner reuses a single message buffer while draining its queues, and limits
each batch to keep other work responsive.

Both queues hold eight copied messages (up to 95-byte topics/256-byte payloads).
`network_publish` returns false immediately for invalid inputs, offline transport
or full queue. Queued messages are dropped if sending fails; this starter does
not automatically replay actions. On observed unavailability, the owner discards
pending TX and application RX so old actions do not replay after reconnect.
There is still a possible disconnect immediately after readiness is checked;
delivery uncertainty is resolved by application synchronisation, not retries of
physical tap events.

`network_ready()` reports Wi-Fi/MQTT/subscription readiness only. It is not a
game-resynchronisation flag. `network_drop_count()` reports owner TX failures,
offline discards and application RX overflow. The lower transport's separate
`mqtt_received_drop_count()` counts callback/FIFO losses; query that only from
the owner. Queue acceptance provides no per-request completion to the caller yet.

Wi-Fi connection failures retry after a delay; MQTT retries/subscriptions remain
in the transport module. Hardware recovery is not yet validated. Snapshot retries,
application acknowledgements, resume gating and a controller lease now exist;
see RECOVERY.md. The actual game snapshot, all-panel barrier and timer preservation
remain application integration work.

The firmware now boots without waiting for USB serial. Opening a monitor later
may miss early logs. It retains the legacy two-board demo topics, not v1 JSON.
Set the real local broker address before flashing; the default hostname remains
a deployment placeholder.

## Validation

Queue tests use stubs to check copying, capacity, invalid/offline requests,
binary receive routing, disconnect cleanup and failed-send accounting. They do
not simulate the scheduler or radio. Run from the repository root:

```powershell
gcc -std=c11 -Wall -Wextra -Werror -Itests/rtos_stubs -Isrc/network tests/network_task_test.c src/network/network_task.c src/network/network_recovery.c -o build/network_task_test.exe
./build/network_task_test.exe
```

On hardware: verify both tasks continue during connection retries, send commands
under sensor/LED load, disconnect/reconnect Wi-Fi and broker, measure stack high
water marks and free heap, then test both nodes together. A successful build and
host tests do not establish timing, latency or physical reliability.

Reference: [Raspberry Pi FreeRTOS examples](https://github.com/raspberrypi/pico-examples/tree/master/pico_w/wifi/freertos).
