#include "network_task.h"
#include "task.h"
#include "queue.h"
#include "network_recovery.h"
#include "pico/time.h"
#include "wifi_manager.h"
#include <string.h>
#include <stdio.h>

// START HERE: read network_task_start(), then network_task(), then transfer_messages().
// A task is a worker scheduled by FreeRTOS. A queue is an inbox that copies data.
// This file moves messages; it never decides what a game command means.
//
// SEND: teammate -> network_publish -> outgoing inbox -> worker -> MQTT -> broker
// READ: broker -> MQTT -> worker -> incoming inbox -> network_receive -> teammate
//
// Initial settings: 8 slots per inbox, priority 3, 2048 stack words (8192 bytes).
// Run the worker roughly every 10 ms; announce online roughly every 1000 ms.
#define QUEUE_LENGTH 8
#define NETWORK_PRIORITY 3
#define NETWORK_STACK_WORDS 2048
#define SERVICE_INTERVAL_MS 10
#define STATUS_INTERVAL_MS 1000
// One outgoing envelope: topic/address, payload/content, delivery options.
// qos 0 = best effort; qos 1 = broker acknowledgement (duplicates are possible).
// retain true = broker saves the latest value for future subscribers.
typedef struct {
    mqtt_message_t message;
    uint8_t qos;
    bool retain;
    bool applied_ack;
} publish_request_t;
// TX = transmit/outgoing inbox. RX = receive/incoming inbox.
// The handles below identify inboxes created at startup.
// static here means these variables belong only to this file and keep their values.
static QueueHandle_t tx_queue, rx_queue;
static bool ready_for_messages, ready_for_game;
static const network_task_config_t *settings;
static network_recovery_t recovery;
static bool recovery_enabled;

static uint32_t drops;

// Count one queued message we discarded. This is useful for diagnosing losses.
// A short critical section prevents another task interrupting the shared update.
static void drop(void) {
    taskENTER_CRITICAL();
    ++drops;
    taskEXIT_CRITICAL();
}
// Let application code read the counter safely. Reading does not reset it.
uint32_t network_drop_count(void) {
    taskENTER_CRITICAL();
    uint32_t value = drops;
    taskEXIT_CRITICAL();
    return value;
}
// Does Wi-Fi/MQTT/subscription setup allow sending? This does NOT mean game synced.
// Protect this read because the network worker can change the flag.
bool network_ready(void) {
    taskENTER_CRITICAL();
    bool ready = ready_for_messages;
    taskEXIT_CRITICAL();
    return ready;
}
// The application checks this before accepting taps. Transport readiness alone
// cannot prove the panel has the latest game state.
bool network_game_ready(void) {
    taskENTER_CRITICAL();
    bool ready = ready_for_game && ready_for_messages;
    taskEXIT_CRITICAL();
    return ready;
}
static void set_game_ready(bool ready) {
    taskENTER_CRITICAL();
    ready_for_game = ready;
    taskEXIT_CRITICAL();
}
static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

// Only our network worker changes readiness; other tasks ask network_ready().
static void set_ready(bool ready) {
    taskENTER_CRITICAL();
    ready_for_messages = ready;
    taskEXIT_CRITICAL();
}

// SEND STEP 1: teammate code leaves an envelope in the outgoing inbox.
// This function does NOT send over Wi-Fi itself; the worker does that later.
// Recovery panels cannot queue gameplay until network_game_ready() is true.
bool network_publish(const char *topic, const void *payload, size_t length,
                     uint8_t qos, bool retain) {
    // Reject offline sends, missing/long topics, publish wildcards (+/#),
    // oversized/missing content, and unsupported QoS. Zero-length content is valid.
    if (!network_game_ready() || !topic || !topic[0] ||
        strlen(topic) > MQTT_TOPIC_MAX_LENGTH || strpbrk(topic, "+#") ||
        length > MQTT_PAYLOAD_MAX_LENGTH || (length && !payload) || qos > 1) return false;
    // Copy everything so the caller can change/reuse their original buffers.
    // memcpy copies exactly length bytes, including zeros in binary messages.
    publish_request_t request = {0};
    strcpy(request.message.topic, topic);
    request.message.length = length;
    if (length) memcpy(request.message.payload, payload, length);
    request.qos = qos;
    request.retain = retain;
    // &request gives FreeRTOS the address to copy FROM. 0 means don't wait for space.
    // true means stored in inbox, NOT delivered to broker or displayed by a panel.
    return xQueueSend(tx_queue, &request, 0) == pdPASS;
}
// App explicitly confirms that it installed the state. A broker PUBACK cannot
// substitute for this. Never call this before the display/state work completes.
bool network_state_applied(const mqtt_message_t *message) {
    if (!network_ready() || !recovery_enabled || !message ||
        strcmp(message->topic, settings->state_topic) ||
        message->length <= RECOVERY_HEADER_SIZE) return false;
    publish_request_t request = {0};
    request.applied_ack = true;
    request.message.length = RECOVERY_HEADER_SIZE;
    memcpy(request.message.payload, message->payload, RECOVERY_HEADER_SIZE);
    return xQueueSend(tx_queue, &request, 0) == pdPASS;
}
bool network_receive(mqtt_message_t *message, TickType_t wait_ticks) {
    // READ STEP 2: teammate code takes the oldest incoming message.
    // Example: network_receive(&message, pdMS_TO_TICKS(20)).
    // &message = "write into my variable here". wait_ticks = maximum wait if empty.
    // Waiting pauses ONLY the caller; other ready tasks continue.
    // false means not started, NULL output, or no message before the timeout.
    return rx_queue && message && xQueueReceive(rx_queue, message, wait_ticks) == pdPASS;
}
static void discard_pending(void) {
    publish_request_t request;
    // Connection unavailable: remove old queued actions that may no longer be valid.
    // Each receive removes an item; 0 makes it return immediately when empty.
    // Reuse one temporary envelope. Incoming items need only its message part.
    while (xQueueReceive(tx_queue, &request, 0) == pdPASS) drop();
    while (xQueueReceive(rx_queue, &request.message, 0) == pdPASS) drop();
}
// Move queued bytes in both directions. Only our worker calls MQTT directly.
// Handle at most one inbox's capacity each way so busy traffic cannot trap us here.
// Reuse one temporary envelope instead of keeping separate send/receive buffers.
static void transfer_messages(void) {
    publish_request_t request;
    // SEND STEP 2: remove outgoing envelopes and give their contents to MQTT.
    for (unsigned i = 0; i < QUEUE_LENGTH &&
         xQueueReceive(tx_queue, &request, 0) == pdPASS; ++i) {
        if (request.applied_ack) {
            if (!recovery_applied(&recovery, request.message.payload,
                                  request.message.length, now_ms())) drop();
            continue;
        }
        // Never send a tap queued just before sync/connection loss began.
        if (!network_game_ready()) { drop(); continue; }
        if (!mqtt_send_ex(request.message.topic, request.message.payload,
                          request.message.length, request.qos, request.retain,
                          NULL, NULL)) drop();
    }
    // READ STEP 1: take complete messages from MQTT and leave them for the app.
    // If the incoming inbox is full, discard the new arrival and count the loss.
    for (unsigned i = 0; i < MQTT_RX_QUEUE_CAPACITY && mqtt_receive(&request.message); ++i) {
        mqtt_message_t *m = &request.message;
        if (recovery_enabled && !strcmp(m->topic, settings->resume_topic)) {
            recovery_resume(&recovery, m->payload, m->length, now_ms());
            continue;
        }
        if (recovery_enabled && !strcmp(m->topic, settings->state_topic)) {
            if (!recovery_state(&recovery, m->payload, m->length, now_ms())) continue;
            set_game_ready(false);
            discard_pending();
            // The header stays attached so the application can acknowledge this
            // exact session/revision. Network code never parses the state body.
            if (xQueueSend(rx_queue, m, 0) != pdPASS) {
                drop();
                recovery_begin(&recovery, now_ms());
            }
        } else if ((!recovery_enabled || recovery.phase == RECOVERY_READY) &&
                   xQueueSend(rx_queue, m, 0) != pdPASS) {
            drop();
            if (recovery_enabled) {
                set_game_ready(false);
                discard_pending();
                recovery_begin(&recovery, now_ms());
            }
        }
    }
}

static void network_task(void *arg) {
    // FreeRTOS gives us back the config address passed when creating this task.
    const network_task_config_t *config = arg;
    // STEP 1: connect Wi-Fi, now that the FreeRTOS scheduler is running.
    // On failure, wait 5 seconds between attempts. Other tasks can still run.
    while (!wifi_init_and_connect(config->ssid, config->password))
        vTaskDelay(pdMS_TO_TICKS(5000));
    // STEP 2: prepare MQTT BEFORE its first connection attempt.
    // Will = broker announces retained "offline" after unexpected connection loss.
    // 10 = keepalive seconds, NOT an exact disconnect deadline.
    // Subscribe = register which topic/filter this board wants to hear, at QoS 1.
    // configASSERT stops execution if either setup call fails (also in Release).
    configASSERT(mqtt_configure_will(config->status_topic, "offline", 10));
    if (config->subscription) configASSERT(mqtt_subscribe_topic_qos(config->subscription, 1));
    for (size_t i = 0; i < config->subscription_count; ++i)
        configASSERT(mqtt_subscribe_topic_qos(config->subscriptions[i], 1));
    if (recovery_enabled) {
        configASSERT(mqtt_subscribe_topic_qos(config->state_topic, 1));
        configASSERT(mqtt_subscribe_topic_qos(config->resume_topic, 1));
    }
    // Remember when we last queued "online" and whether we were ready last pass.
    TickType_t last_status = 0;
    bool was_ready = false;
    uint32_t generation = 0;
    uint32_t transport_drops = mqtt_received_drop_count();
    // STEP 3: repeat forever. FreeRTOS schedules this alongside the other tasks.
    for (;;) {
        bool wifi = is_wifi_connected();
        // Despite its name, init_mqtt_client also services retry/subscription work.
        // Keep calling it even after connecting. It takes the required lwIP lock.
        if (wifi) init_mqtt_client(config->client_id);
        bool ready = wifi && mqtt_is_ready();
        if (!ready) {
            // Stop accepting new work; clear actions queued before connection loss.
            set_ready(false);
            set_game_ready(false);
            if (recovery_enabled) recovery_disconnect(&recovery);
            discard_pending();
            if (was_ready) printf("Network unavailable; pending application messages discarded.\n");
            was_ready = false;
            if (!wifi) {
                // Wi-Fi lost: this same worker tries joining again.
                // Connection waits pause this task, not the whole Pico.
                wifi_init_and_connect(config->ssid, config->password);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            uint32_t current_generation = mqtt_connection_generation();
            if (!was_ready || generation != current_generation) {
                set_ready(false);
                set_game_ready(false);
                discard_pending();
                if (recovery_enabled) recovery_begin(&recovery, now_ms());
                generation = current_generation;
            }
            set_ready(true);
            if (!recovery_enabled) set_game_ready(true);
            // Announce "online" on connection, then once per second (retrying failures).
            // 6 = bytes in "online", excluding the terminating zero.
            // This is presence, NOT a game heartbeat or "state applied" confirmation.
            TickType_t now = xTaskGetTickCount();
            if (!was_ready || now - last_status >= pdMS_TO_TICKS(STATUS_INTERVAL_MS)) {
                if (mqtt_send_ex(config->status_topic, "online", 6, 1, true, NULL, NULL))
                    last_status = now;
            }
            was_ready = true;
            // Even the lower MQTT inbox may overflow. Any observed loss makes
            // application state uncertain, so recovery panels request a snapshot.
            uint32_t current_drops = mqtt_received_drop_count();
            bool lost_messages = current_drops != transport_drops;
            transport_drops = current_drops;
            if (recovery_enabled && lost_messages) recovery_begin(&recovery, now_ms());
            if (recovery_enabled && (lost_messages || recovery_tick(&recovery, now_ms()))) {
                set_game_ready(false);
                discard_pending();
            }
            transfer_messages();
            if (recovery_enabled) {
                uint8_t header[RECOVERY_HEADER_SIZE];
                size_t length = recovery_outgoing(&recovery, now_ms(), header);
                if (length && !mqtt_send_ex(length == 8 ? config->sync_topic : config->applied_topic,
                                            header, length, 1, false, NULL, NULL)) drop();
                set_game_ready(recovery.phase == RECOVERY_READY);
            }
        }
        // Pause this worker for about 10 ms so other ready tasks can run.
        // pdMS_TO_TICKS converts milliseconds to FreeRTOS time units.
        vTaskDelay(pdMS_TO_TICKS(SERVICE_INTERVAL_MS));
    }
}
// Validate startup strings before the worker starts. Bad configuration returns
// false here instead of crashing later inside a running task.
static bool valid_config_topic(const char *topic, bool filter) {
    if (!topic || !topic[0] || strlen(topic) > MQTT_TOPIC_MAX_LENGTH) return false;
    for (size_t i = 0; topic[i]; ++i) {
        if (topic[i] != '+' && topic[i] != '#') continue;
        if (!filter || (i && topic[i-1] != '/')) return false;
        if (topic[i] == '#' && topic[i+1]) return false;
        if (topic[i] == '+' && topic[i+1] && topic[i+1] != '/') return false;
    }
    return true;
}
// main() calls this ONCE before vTaskStartScheduler().
// It creates the inboxes and worker; it does not connect to Wi-Fi immediately.
bool network_task_start(const network_task_config_t *config) {
    // Reject a second start or missing settings. Config/strings must stay alive:
    // the worker keeps their addresses, rather than copying the config.
    if (tx_queue || !config || !config->ssid || !config->password || !config->client_id ||
        !config->status_topic) return false;
    if (config->subscription_count > MQTT_MAX_SUBSCRIPTIONS ||
        (config->subscription_count && !config->subscriptions)) return false;
    if (!config->ssid[0] || !config->password[0] || !config->client_id[0] ||
        !valid_config_topic(config->status_topic, false) ||
        (config->subscription && !valid_config_topic(config->subscription, true))) return false;
    size_t count = config->subscription_count + (config->subscription != NULL);
    bool enabled = config->sync_topic != NULL;
    if (enabled != (config->state_topic != NULL) ||
        enabled != (config->applied_topic != NULL) ||
        enabled != (config->resume_topic != NULL) || (enabled && !config->boot_nonce)) return false;
    if (enabled) {
        const char *topics[] = {config->sync_topic, config->state_topic,
                               config->applied_topic, config->resume_topic, config->status_topic};
        for (size_t i = 0; i < 5; ++i) {
            if (!valid_config_topic(topics[i], false)) return false;
            for (size_t j = 0; j < i; ++j)
                if (!strcmp(topics[i], topics[j])) return false;
        }
    }
    if (count + (enabled ? 2u : 0u) > MQTT_MAX_SUBSCRIPTIONS) return false;
    for (size_t i = 0; i < config->subscription_count; ++i)
        if (!valid_config_topic(config->subscriptions[i], true)) return false;
    settings = config;
    recovery_enabled = enabled;
    recovery_init(&recovery, config->boot_nonce);
    // Allocate space for 8 outgoing envelopes and 8 incoming messages.
    tx_queue = xQueueCreate(QUEUE_LENGTH, sizeof(publish_request_t));
    rx_queue = xQueueCreate(QUEUE_LENGTH, sizeof(mqtt_message_t));
    // Register the worker with FreeRTOS. It runs after the scheduler starts.
    // Priority 3 is above the demo Application Task's priority 2.
    if (tx_queue && rx_queue &&
        xTaskCreate(network_task, "Network", NETWORK_STACK_WORDS, (void *)config, NETWORK_PRIORITY, NULL) == pdPASS)
        return true;
    // Startup failed: free whichever inboxes were created, then report false.
    if (tx_queue) vQueueDelete(tx_queue);
    if (rx_queue) vQueueDelete(rx_queue);
    tx_queue = rx_queue = NULL;
    return false;
}
