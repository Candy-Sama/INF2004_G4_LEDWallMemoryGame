#ifndef NETWORK_TASK_H
#define NETWORK_TASK_H
#include "FreeRTOS.h"
#include "mqtt_handler.h"

// TEAMMATE INTERFACE: startup creates the worker; tasks use publish/receive.
// These calls are for tasks, not interrupt handlers (ISRs).
//
// main() supplies these settings once. Declare the config as static so it and
// its strings stay alive: the worker keeps using their addresses.
typedef struct {
    // Wi-Fi name/password, plus a UNIQUE MQTT client name for this physical board.
    const char *ssid, *password, *client_id;
    // Topic/filter to listen to; topic for this board's online/offline announcements.
    const char *subscription, *status_topic;
    // Optional extra filters, e.g. status + taps + recovery on a controller.
    const char *const *subscriptions;
    size_t subscription_count;
    // Optional recovery. Set ALL four topics to enable, or leave ALL NULL.
    // Topics are unique to this panel. State/resume are subscribed automatically.
    const char *sync_topic, *state_topic, *applied_topic, *resume_topic;
    uint64_t boot_nonce; // Fresh random value on every reboot; nonzero.

} network_task_config_t;
// Call once before vTaskStartScheduler(). true = worker/inboxes created,
// NOT "Wi-Fi connected". Other tasks do not need their own config or start call.
bool network_task_start(const network_task_config_t *config);

// Leave an outgoing message in the worker's inbox. Returns without waiting.
// topic = address; payload = content bytes; length = number of content bytes.
// qos 0 = best effort; qos 1 = broker acknowledgement (duplicates are possible).
// retain true = remember latest state; false = don't save an event such as a tap.
// true = copied into queue, NOT "delivered". false = offline/syncing, invalid, or full.
// Caller can reuse their buffers afterwards. Queued work is discarded if offline.
bool network_publish(const char *topic, const void *payload, size_t length,
                     uint8_t qos, bool retain);
// Take the oldest incoming message and fill your variable:
//   mqtt_message_t message;
//   bool received = network_receive(&message, pdMS_TO_TICKS(20));
// &message = address of your variable, so this function can write into it.
// wait_ticks = maximum waiting time IF inbox is empty:
//   0                -> check once; no waiting
//   pdMS_TO_TICKS(20) -> wait up to about 20 ms; return sooner on arrival
//   portMAX_DELAY    -> wait indefinitely in our configuration
// Waiting pauses ONLY this caller. Other ready tasks continue running.
// true = copied a message; false = timeout, NULL output, or not started.
// Only read message on true. Payload is bytes, NOT a zero-terminated C string.
// ONE application receiver takes messages and routes them to teammates;
// multiple receivers would take messages away from one another.
bool network_receive(mqtt_message_t *message, TickType_t wait_ticks);
// After applying the COMPLETE snapshot (including assignment/display), pass the
// original state message here. We copy its recovery header, not its game payload.
// true = queued acknowledgement; gameplay still waits for controller resume.
bool network_state_applied(const mqtt_message_t *state_message);
// Transport ready AND controller has authorised this panel to play.
// Recovery-disabled legacy configurations behave like network_ready().
bool network_game_ready(void);

// Is Wi-Fi/MQTT/subscription setup ready? Does NOT mean the game is synchronised.
bool network_ready(void); // Transport readiness, not game synchronisation.
// Count queued messages discarded: failed sends, offline clearing, incoming full.
// Excludes rejected publish calls and the separate lower MQTT loss counter.
uint32_t network_drop_count(void); // TX failures and application RX overflow.
#endif
