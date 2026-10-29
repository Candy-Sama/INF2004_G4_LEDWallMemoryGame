// net.c  (Mustaqim's module: Wi-Fi, MQTT, reconnection)
// Moved out of main.c with no logic changes. Edits are marked // CHANGED.

#include <stdio.h>
#include <string.h>
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

// Application-specific interpretation stays out of the MQTT transport.
static void apply_received_command(void) {
    mqtt_message_t message;
    // Bound each batch so incoming traffic cannot starve sensor sampling.
    for (size_t i = 0; i < MQTT_RX_QUEUE_CAPACITY && mqtt_receive(&message); ++i) {
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