// net.c  (Mustaqim's module: Wi-Fi, MQTT, reconnection)
// Moved out of main.c with no logic changes. Edits are marked // CHANGED.

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "net.h"
#include "node_config.h"
#include "node_identity.h"
#include "wifi_manager.h"
#include "wifi_creds.h"
#include "mqtt_handler.h"
#include "imagedata.h"
#include "display.h"       // CHANGED: all drawing goes through the display task
#include "sensor.h"        // CHANGED: app_event_t + sensor queue
#include "game_logic.h"    // CHANGED: MQTT_TAP/STATE/REVEAL topic macros
#include "game_task.h"     // CHANGED: IS_GAME_MASTER + the two game hooks
#include "imu_orientation.h"      // shared rotation payload check
#include "node_registry.h"

#define NET_TASK_PRIORITY 2     // CHANGED: moved from main.c
#define NET_TASK_STACK    2048  // CHANGED: moved from main.c


static QueueHandle_t net_event_q;   // CHANGED: set in net_start() from the sensor module

static void send_movement(void) {
    static bool send_heart_next = true;
    if (!is_wifi_connected() || !is_mqtt_connected()) {
        printf("Movement ignored: network is disconnected.\n");
        return;
    }
    const char *card = send_heart_next ? "Heart" : "Dark Heart";
    if (mqtt_send(identity_command_topic(), card, strlen(card))) {
        printf("Movement: queued %s to %s\n", card, identity_command_topic());
        display_show_image(send_heart_next ? heartCard_bin : heartCardDark_bin);  // CHANGED
        send_heart_next = !send_heart_next;
    } else {
        printf("Movement: publish failed; try again.\n");
    }
}

static void handle_sensor_events(void) {
    app_event_t evt;
    while (xQueueReceive(net_event_q, &evt, 0) == pdTRUE) {
        if (evt == APP_EVT_MOTION) send_movement();
    }
}

static bool is_board_id_text(const char *s) {            // exactly 16 hex chars
    if (strlen(s) != IDENTITY_BOARD_ID_LEN - 1) return false;
    for (size_t i = 0; s[i]; ++i) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

// Master: a board without a number asked to join. Reply with its number.
static void handle_join_request(const mqtt_message_t *message) {
    if (message->length != IDENTITY_BOARD_ID_LEN - 1) return;
    char board_id[IDENTITY_BOARD_ID_LEN];
    memcpy(board_id, (const char *)message->payload, IDENTITY_BOARD_ID_LEN - 1);
    board_id[IDENTITY_BOARD_ID_LEN - 1] = '\0';
    if (!is_board_id_text(board_id)) return;

    int node_id = registry_assign(board_id);
    if (node_id == 0) { printf("Join: grid full, rejected %s\n", board_id); return; }

    char reply_topic[IDENTITY_TOPIC_LEN];
    char reply[4];
    if (!identity_assign_topic_for(board_id, reply_topic, sizeof reply_topic)) return;
    int n = snprintf(reply, sizeof reply, "%d", node_id);
    if (mqtt_send(reply_topic, reply, (size_t)n)) {
        printf("Join: %s -> node %d\n", board_id, node_id);
    } else {
        printf("Join: reply to %s failed; it will retry.\n", board_id);
    }
}

// Unassigned board: ask the master for a number, blocking until one arrives.
// Uses a temporary MQTT session with NO Last Will (the will topic needs a node number).
static void join_grid(const char *client_id) {
    if (!mqtt_subscribe_topic(identity_assign_topic())) {
        printf("Could not register the assign topic.\n");
        vTaskDelete(NULL);
    }
    TickType_t next_join = xTaskGetTickCount();

    while (!identity_is_assigned()) {
        if (!is_wifi_connected()) {
            mqtt_reset_connection();
            wifi_init_and_connect(WIFI_SSID, WIFI_PASSWORD);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        init_mqtt_client(client_id);

        if (mqtt_is_ready()) {                       // our assign subscription is active
            TickType_t now = xTaskGetTickCount();
            if ((int32_t)(now - next_join) >= 0) {
                mqtt_send(MQTT_JOIN_TOPIC, identity_board_id(), strlen(identity_board_id()));
                printf("Joining as %s...\n", identity_board_id());
                next_join = now + pdMS_TO_TICKS(2000);
            }
            mqtt_message_t message;
            while (mqtt_receive(&message)) {
                if (strcmp(message.topic, identity_assign_topic()) != 0) continue;
                const char *p = (const char *)message.payload;
                if (message.length == 0 || message.length > 2) continue;
                int id = 0;
                for (size_t i = 0; i < message.length; ++i) {
                    if (p[i] < '0' || p[i] > '9') { id = 0; break; }
                    id = id * 10 + (p[i] - '0');
                }
                identity_set_node_id(id);            // ignores out-of-range values
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("Assigned panel %d\n", identity_node_id());
    mqtt_reset_connection();   // drop the will-less session; startup below reconnects properly
}

// Application-specific interpretation stays out of the MQTT transport.
static void apply_received_command(void) {
    mqtt_message_t message;
    // Bound each batch so incoming traffic cannot starve sensor sampling.
    for (size_t i = 0; i < MQTT_RX_QUEUE_CAPACITY && mqtt_receive(&message); ++i) {
        if (IS_GAME_MASTER && strcmp(message.topic, MQTT_JOIN_TOPIC) == 0) {
            handle_join_request(&message);
            continue;
        }
        // CHANGED: tap from a slave, game master only. Handed to the game module.
        if (IS_GAME_MASTER && strcmp(message.topic, MQTT_TAP_TOPIC) == 0) {
            game_handle_tap_payload((const uint8_t *)message.payload, message.length);
            continue;
        }
        #if ENABLE_ORIENTATION_SYNC
            // Another panel (or this one, echoed back) was rotated: follow it.
        if (strcmp(message.topic, MQTT_ORIENTATION_TOPIC) == 0) {
            uint8_t turns;
            if (orientation_sync_decode((const uint8_t *)message.payload, message.length, &turns)) {
                display_set_rotation(turns);
                printf("Orientation sync: rotation set to %u\n", (unsigned)turns);
            } else {
                printf("Orientation sync: ignored invalid payload (%u bytes).\n",
                        (unsigned)message.length);
            }
            continue;
        }
        #endif

        if (strcmp(message.topic, identity_command_topic()) != 0) continue;
        if (message.length == 5 && memcmp(message.payload, "Heart", 5) == 0) {
            display_show_image(heartCard_bin);                       // CHANGED
            printf("LED command applied: Heart\n");
        } else if (message.length == 10 && memcmp(message.payload, "Dark Heart", 10) == 0) {
            display_show_image(heartCardDark_bin);                   // CHANGED
            printf("LED command applied: Dark Heart\n");
        } else if (message.length == 3 && memcmp(message.payload, "OFF", 3) == 0) {
            display_show_colour(0, 0, 0);                            // CHANGED (was ledmatrix_fill)
            printf("LED command applied: OFF\n");
        } else {
            printf("Demo: unrecognised payload on %s (%u bytes).\n",
                   message.topic, (unsigned)message.length);
        }
    }
    static uint32_t previous_drops;
    uint32_t drops = mqtt_received_drop_count();
    if (drops != previous_drops) {
        printf("MQTT: %lu received messages discarded in total.\n", (unsigned long)drops);
        previous_drops = drops;
    }
}

static void publish_status(void) {
    static TickType_t next_publish;
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - next_publish) >= 0) {
        mqtt_send_ex(identity_status_topic(), MQTT_STATUS_MESSAGE,
                     strlen(MQTT_STATUS_MESSAGE), 1, true, NULL, NULL);
        next_publish = now + pdMS_TO_TICKS(1000);
    }
}

#if ENABLE_ORIENTATION_SYNC
// Share a rotation caused by this panel's own sensor. Retained QoS 1 state (not an
// event), so panels that boot or reconnect later still learn the current rotation.
// If MQTT is not ready the change stays pending in the sensor module.
static void publish_orientation(void) {
    uint8_t turns;
    if (!sensor_take_orientation_change(&turns)) return;
    if (!mqtt_send_ex(MQTT_ORIENTATION_TOPIC, &turns, 1, 1, true, NULL, NULL)) {
        printf("Orientation sync: publish failed.\n");
    } else {
        printf("Orientation sync: shared rotation %u\n", (unsigned)turns);
    }
}
#endif


static void report_wifi_status(bool connected) {
    printf("Wi-Fi %s.\n", connected ? "connected" : "disconnected");
    // Send a status instruction to the display here (display_show_*).
    // The Display Task will render it.
}

static void net_task(void *arg) {
    (void)arg;

    // Client IDs identify physical boards; the node identity chooses their MQTT topics.
    char board_id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    static char client_id[32];   // must stay valid for the lifetime of the connection
    pico_get_unique_board_id_string(board_id, sizeof(board_id));
    snprintf(client_id, sizeof(client_id), "SE4_%s", board_id);
    printf("MQTT client: %s\nListen: %s\n", client_id, identity_command_topic());

    // Wi-Fi setup must run inside a task when using the FreeRTOS cyw43 arch.
    while (!wifi_init_and_connect(WIFI_SSID, WIFI_PASSWORD)) {
        printf("Wi-Fi setup failed. Retrying in 5 s...\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
    if (!identity_is_assigned()) join_grid(client_id);   // NEW: blocks until we have a number
    if (IS_GAME_MASTER) registry_init();                 // NEW: master seeds the registry

    // Broker publishes retained 'offline' if this connection disappears.
    if (!mqtt_configure_will(identity_status_topic(), "offline", 10)) {
        printf("Could not configure MQTT Last Will.\n");
        vTaskDelete(NULL);
    }
    if (!mqtt_subscribe_topic(identity_command_topic())) {
        printf("Could not register the command topic.\n");
        vTaskDelete(NULL);
    }

    // Game master also listens for taps.
    if (IS_GAME_MASTER && !mqtt_subscribe_topic(MQTT_TAP_TOPIC)) {
        printf("Could not register the tap topic.\n");
        vTaskDelete(NULL);
    }

    if (IS_GAME_MASTER && !mqtt_subscribe_topic(MQTT_JOIN_TOPIC)) {
        printf("Could not register the join topic.\n");
        vTaskDelete(NULL);
    }

    #if ENABLE_ORIENTATION_SYNC
    // Every panel follows the shared rotation.
    if (!mqtt_subscribe_topic_qos(MQTT_ORIENTATION_TOPIC, 1)) {
        printf("Could not register the orientation topic.\n");
        vTaskDelete(NULL);
    }
    #endif

    bool display_started = false;
    bool wifi_was_connected = true;

    bool mqtt_was_ready = false;
    TickType_t next_wifi_retry = xTaskGetTickCount();   // Reconnection Protocol part 1

    for (;;) {
        bool wifi_connected = is_wifi_connected();
        // Wi-Fi loss block
        if (wifi_connected != wifi_was_connected) {
            report_wifi_status(wifi_connected);
            if (!wifi_connected) {
                mqtt_reset_connection();
                mqtt_was_ready = false;
                next_wifi_retry = xTaskGetTickCount();
            }
            wifi_was_connected = wifi_connected;
        }

        // Reconnection Protocol part 2 (retry block)
        if (!wifi_connected) {
            TickType_t now = xTaskGetTickCount();
            if ((int32_t)(now - next_wifi_retry) >= 0) {
                printf("Attempting Wi-Fi reconnection...\n");
                bool reconnected = wifi_init_and_connect(WIFI_SSID, WIFI_PASSWORD);

                next_wifi_retry = xTaskGetTickCount() + pdMS_TO_TICKS(5000);

                // Discard movements accumulated during the connection attempt.
                xQueueReset(net_event_q);

                if (!reconnected) {
                    printf("Wi-Fi retry failed. Waiting 5 seconds.\n");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Wi-Fi is available here: the disconnected branch uses continue.
        init_mqtt_client(client_id);
        bool mqtt_ready = mqtt_is_ready();

        if (mqtt_ready) {
            if (!mqtt_was_ready) {
                // Just became ready: discard inputs collected before readiness.
                xQueueReset(net_event_q);
                printf("MQTT connected and subscriptions ready.\n");
            } else {
                handle_sensor_events();
            }

            if (!display_started) {
                display_show_own_number();                      // CHANGED
                display_started = true;
            }

            apply_received_command();
            #if ENABLE_ORIENTATION_SYNC
            publish_orientation();
            #endif

            publish_status();
            if (IS_GAME_MASTER) game_publish_output();          // CHANGED (was publish_game_output)
        } else {
            // Do not save physical inputs for later replay.
            xQueueReset(net_event_q);
        }

        mqtt_was_ready = mqtt_ready;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool net_start(void) {                                          // CHANGED: new
    net_event_q = sensor_get_event_queue();
    if (!net_event_q) return false;
    return xTaskCreate(net_task, "net", NET_TASK_STACK, NULL, NET_TASK_PRIORITY, NULL) == pdPASS;
}