# Reconnection test results

Validated on the MQTT branch on 22 October 2026. The tests compile the actual
network source against fake SDK calls and controlled callback/clock behaviour.
No Pico, broker, Wi-Fi adapter or running FreeRTOS scheduler is involved.

## Run on Windows

From the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/run_host_tests.ps1
```

The process-only execution-policy option allows this checked-in script to run
without changing the machine's execution-policy settings. The default compiler
is `C:/msys64/mingw64/bin/gcc.exe`; override it using `-Compiler` if needed.
The runner builds fresh test executables with `-Wall -Wextra -Werror`, stops on
compilation/test failure, and saves each suite's output in `build/host-tests/`.

## Results

| Suite | Result | Coverage |
| --- | --- | --- |
| mqtt_handler | PASS | Existing input/binary/FIFO/QoS/retain/Will tests plus reset and DNS recovery cases below |
| wifi_manager | PASS | Input validation, initialisation failure, failed joins, simulated established-link loss and retry without SDK reinitialisation |
| motion_detector | PASS | Existing motion/cooldown/gap/wrapping-clock cases |
| wifi_setup | PASS | Optional advanced serial input, retry, bounds and no firmware password echo |

The MQTT reset tests verify:

- Reset is safe before a client is allocated.
- Connected reset calls disconnect under the network lock. The fake disconnect
  intentionally does not call the connection callback, matching the real API.
- Complete and partially received old messages are dropped and counted once.
- Offline publishing is rejected; the client is allocated only once.
- Retry does not begin at 4999 ms but is allowed at 5000 ms after reset.
- Previously registered topics and QoS values are restored. Readiness remains
  false until both restored subscriptions have been acknowledged.
- Fresh binary messages are still received correctly after reset/reconnect.
- An old DNS success after reset cannot start a connection.
- An old DNS success or failure arriving during a newer lookup cannot start an
  old connection, cancel the new attempt, or overwrite the fresh client ID.
- Repeated servicing does not start overlapping DNS lookups.
- Reset during connect or while awaiting SUBACK permits a new connection.
- Failed DNS initiation, resolver failure and immediate connect failure remain
  retryable. Unsigned elapsed time handles clock wraparound.
- Last Will and keepalive configuration survive reconnect, as asserted by the
  mock connect function on every attempt.

Expected failure-path logs (for example `DNS resolution failed` or publish
failure) are deliberately injected test scenarios, not failures of the suite.
A failing assertion or nonzero executable/compiler exit fails the runner.

## What these results do not establish

- Actual Wi-Fi reassociation, IP acquisition, AP roaming or broker reachability.
- FreeRTOS scheduling, the main task loop's queue-gating timing, stack sufficiency
  or physical input-to-display latency.
- Broker Last Will detection timing, real QoS packet retransmission or callbacks
  cancelled by the real SDK. The stub follows the documented cancellation rule.
- Game-state resynchronisation or safe gameplay resumption after transport ready.

The firmware build also passes. No production source changes were needed for
these tests. Hardware sign-off remains pending: disable/re-enable Wi-Fi without
resetting the Pico, restart the broker, verify subscriptions/presence return,
and verify input from the outage is not replayed after readiness returns.
