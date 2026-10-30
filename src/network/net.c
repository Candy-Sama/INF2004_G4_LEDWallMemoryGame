// net.c  (Mustaqim's module: Wi-Fi, MQTT, reconnection)
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
#include "display.h"
#include "sensor.h"
#include "imu_orientation.h"

#define NET_TASK_PRIORITY 2
#define NET_TASK_STACK    2048
#define NET_PUB_QUEUE_LEN 8

static QueueHandle_t net_event_q;

/* ---------------------------------------------------------- API: registry */

typedef struct {
    char             filter[MQTT_TOPIC_MAX_LENGTH + 1];
    net_rx_handler_t fn;
} net_handler_t;

typedef struct {
    char    topic[MQTT_TOPIC_MAX_LENGTH + 1];
    uint8_t data[NET_PUB_MAX_PAYLOAD];
    uint8_t len;
    bool    retain;
} net_pub_t;

static net_handler_t s_handlers[NET_MAX_HANDLERS];
static size_t        s_handler_count;
static QueueHandle_t s_pub_q;

bool net_register_handler(const char *filter, net_rx_handler_t fn) {
    if (!filter || !fn || s_handler_count >= NET_MAX_HANDLERS) return false;
    if (strlen(filter) > MQTT_TOPIC_MAX_LENGTH) return false;
    strcpy(s_handlers[s_handler_count].filter, filter);
    s_handlers[s_handler_count].fn = fn;
    s_handler_count++;
    return true;
}

bool net_publish(const char *topic, const void *data, size_t len, bool retain) {
    if (!s_pub_q || !topic || len > NET_PUB_MAX_PAYLOAD || (len > 0 && !data)) return false;
    if (strlen(topic) > MQTT_TOPIC_MAX_LENGTH) return false;
    net_pub_t m;
    strcpy(m.topic, topic);
    if (len) memcpy(m.data, data, len);
    m.len    = (uint8_t)len;
    m.retain = retain;
    return xQueueSend(s_pub_q, &m, 0) == pdTRUE;
}

// MQTT filter match: '+' = one level, '#' = the rest.
static bool topic_matches(const char *f, const char *t) {
    while (*f) {
        if (*f == '#') return true;
        if (*f == '+') {
            while (*t && *t != '/') t++;
            f++;
        } else {
            if (*f != *t) return false;
            f++; t++;
        }
    }
    return *t == '\0';
}

// Returns true if at least one registered handler took the message.
static bool dispatch_to_handlers(const mqtt_message_t *msg) {
    bool handled = false;
    for (size_t i = 0; i < s_handler_count; i++) {
        if (topic_matches(s_handlers[i].filter, msg->topic)) {
            s_handlers[i].fn(msg->topic, msg->payload, msg->length);
            handled = true;
        }
    }
    return handled;
}

static void drain_publish_queue(void) {
    net_pub_t m;
    while (xQueueReceive(s_pub_q, &m, 0) == pdTRUE) {
        if (!mqtt_send_ex(m.topic, m.data, m.len, 1, m.retain, NULL, NULL)) {
            printf("Net: publish to %s failed.\n", m.topic);
        }
    }
}

/* ------------------------------------------------------------ existing code */

static void send_movement(void) {
    static bool send_clover_next = true;
    if (!is_wifi_connected() || !is_mqtt_connected()) {
        printf("Movement ignored: network is disconnected.\n");
        return;
    }
    const char *card = send_clover_next ? "diamond" : "Clover";
    if (mqtt_send(identity_command_topic(), card, strlen(card))) {
        printf("Movement: queued %s to %s\n", card, identity_command_topic());
        display_show_image(send_clover_next ? diamondCard_bin : cloverCard_bin);
        send_clover_next = !send_clover_next;
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

static void apply_received_command(void) {
    mqtt_message_t message;
    for (size_t i = 0; i < MQTT_RX_QUEUE_CAPACITY && mqtt_receive(&message); ++i) {

        if (dispatch_to_handlers(&message)) continue;          // API

#if ENABLE_ORIENTATION_SYNC
        if (strcmp(message.topic, MQTT_ORIENTATION_TOPIC) == 0) {
            uint8_t turns;
            if (orientation_sync_decode(message.payload, message.length, &turns)) {
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
        if (message.length == 7 && memcmp(message.payload, "diamond", 7) == 0) {
            display_show_image(diamondCard_bin);
            printf("LED command applied: Diamond\n");
        } else if (message.length == 6 && memcmp(message.payload, "Clover", 6) == 0) {
            display_show_image(cloverCard_bin);
            printf("LED command applied: Clover\n");
        } else if (message.length == 3 && memcmp(message.payload, "OFF", 3) == 0) {
            display_show_colour(0, 0, 0);
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
}

static void net_task(void *arg) {
    (void)arg;

    char board_id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    static char client_id[32];
    pico_get_unique_board_id_string(board_id, sizeof(board_id));
    snprintf(client_id, sizeof(client_id), "SE4_%s", board_id);
    printf("MQTT client: %s\nListen: %s\n", client_id, identity_command_topic());

    while (!wifi_init_and_connect(WIFI_SSID, WIFI_PASSWORD)) {
        printf("Wi-Fi setup failed. Retrying in 5 s...\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    if (!mqtt_configure_will(identity_status_topic(), "offline", 10)) {
        printf("Could not configure MQTT Last Will.\n");
        vTaskDelete(NULL);
    }
    if (!mqtt_subscribe_topic(identity_command_topic())) {
        printf("Could not register the command topic.\n");
        vTaskDelete(NULL);
    }

    // API: subscribe once for every module that registered a handler.
    for (size_t i = 0; i < s_handler_count; i++) {
        if (!mqtt_subscribe_topic(s_handlers[i].filter)) {
            printf("Could not register %s.\n", s_handlers[i].filter);
            vTaskDelete(NULL);
        }
    }

#if ENABLE_ORIENTATION_SYNC
    if (!mqtt_subscribe_topic_qos(MQTT_ORIENTATION_TOPIC, 1)) {
        printf("Could not register the orientation topic.\n");
        vTaskDelete(NULL);
    }
#endif

    bool display_started = false;
    bool wifi_was_connected = true;
    bool mqtt_was_ready = false;
    TickType_t next_wifi_retry = xTaskGetTickCount();

    for (;;) {
        bool wifi_connected = is_wifi_connected();
        if (wifi_connected != wifi_was_connected) {
            report_wifi_status(wifi_connected);
            if (!wifi_connected) {
                mqtt_reset_connection();
                mqtt_was_ready = false;
                next_wifi_retry = xTaskGetTickCount();
            }
            wifi_was_connected = wifi_connected;
        }

        if (!wifi_connected) {
            TickType_t now = xTaskGetTickCount();
            if ((int32_t)(now - next_wifi_retry) >= 0) {
                printf("Attempting Wi-Fi reconnection...\n");
                bool reconnected = wifi_init_and_connect(WIFI_SSID, WIFI_PASSWORD);
                next_wifi_retry = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
                xQueueReset(net_event_q);
                xQueueReset(s_pub_q);                           // API: drop stale publishes
                if (!reconnected) printf("Wi-Fi retry failed. Waiting 5 seconds.\n");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        init_mqtt_client(client_id);
        bool mqtt_ready = mqtt_is_ready();

        if (mqtt_ready) {
            if (!mqtt_was_ready) {
                xQueueReset(net_event_q);
                printf("MQTT connected and subscriptions ready.\n");
            } else {
                handle_sensor_events();
            }

            if (!display_started) {
                display_show_own_number();
                display_started = true;
            }

            apply_received_command();
#if ENABLE_ORIENTATION_SYNC
            publish_orientation();
#endif
            publish_status();
            drain_publish_queue();                              // API
        } else {
            xQueueReset(net_event_q);
            xQueueReset(s_pub_q);                               // API: no stale replay
        }

        mqtt_was_ready = mqtt_ready;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool net_start(void) {
    net_event_q = sensor_get_event_queue();
    s_pub_q = xQueueCreate(NET_PUB_QUEUE_LEN, sizeof(net_pub_t));   // API
    if (!net_event_q || !s_pub_q) return false;
    return xTaskCreate(net_task, "net", NET_TASK_STACK, NULL, NET_TASK_PRIORITY, NULL) == pdPASS;
}