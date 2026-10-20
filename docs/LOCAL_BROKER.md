# First local MQTT test

**Current firmware:** the demo now requires snapshot/applied/resume recovery
before accepting colour commands or publishing movement. Run the example
controller from [RECOVERY.md](RECOVERY.md) for the appropriate node ID. The
standalone terminal learning exercises still work without firmware or Picos.


The broker is a program on your PC, not on the Pico. Keep the PC and both Picos
on the same private test router; disable client isolation on that router.

1. Install [Eclipse Mosquitto](https://mosquitto.org/download/) on the broker PC.
   Its command-line tools include `mosquitto_sub` and `mosquitto_pub`.
2. Run `ipconfig` and find the IPv4 address of the adapter connected to that router.
   Reserve that address in the router so it does not change. This is the address
   to put in `config/network_config.h` as `MQTT_BROKER_HOST`.
   `memorywall-broker.lan` is a placeholder unless you configured local DNS.
3. For an isolated prototype, create a local `mosquitto-test.conf` containing:

   ```conf
   listener 1883
   allow_anonymous true
   ```

   Start it from a terminal: `mosquitto -c mosquitto-test.conf -v`.
   If an installed Mosquitto service already occupies 1883, configure that
   service or stop it before starting a separate instance. Allow inbound TCP
   1883 on the private test-network firewall profile. Do not forward the port
   through the router. Anonymous mode permits everyone on that network to send
   commands; use it only on the dedicated prototype network. For a shared
   network, add broker authentication/ACLs and firmware credential configuration
   before deployment; a local broker alone does not authenticate publishers.
4. In another terminal, replace the example address below with the actual PC IP:

   ```powershell
   mosquitto_sub -h 192.168.1.50 -t 'se4/memorygame/#' -q 1 -v
   ```

5. Rebuild and flash a distinct NODE_ID for each board. The FreeRTOS demo starts automatically; open USB serial to observe it. Look for its command-topic subscription and retained online status.
6. Send a command to Node 2 (no `-r`: commands should not be retained):

   ```powershell
   mosquitto_pub -h 192.168.1.50 -t 'se4/memorygame/node2/command' -m GREEN -q 0
   ```

7. Start another subscriber. It should immediately see the retained `online`
   values, even though it subscribed after those publications.
8. Unplug one Pico. After broker loss detection, its status should become
   `offline`. Expect seconds, not 200 ms; the demo keepalive is 10 seconds.
   Reconnect it: online should replace offline. The demo
   does not yet pause a game or perform the v1 state handshake.

Rebuilding overwrites `build/LED_MEMORY_GAME.uf2`; old named UF2 snapshots are
not refreshed automatically. The full JSON protocol is in [MQTT_PROTOCOL.md](MQTT_PROTOCOL.md).
The short `online`/`offline` strings above belong only to the legacy demo.

See [Mosquitto authentication documentation](https://www.mosquitto.org/documentation/authentication-methods/)
for listener access settings. These instructions prepare a test; they do not
install software or change your PC firewall automatically.
