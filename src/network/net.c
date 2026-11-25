// net.c  (Mustaqim's module: Wi-Fi, MQTT, reconnection; Nabillah's join/registry ; Afiqah's synchronized animation)
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
#include "display.h"
#include "buzzer.h"
#include "sensor.h"
#include "game_logic.h"
#include "imu_orientation.h"
#include "node_registry.h"
#include "clock_sync.h"

#define NET_TASK_PRIORITY    2
#define NET_TASK_STACK       2048
#define NET_PUB_QUEUE_LEN    32
#define REGISTRY_MASTER_NODE 1
#define IS_REGISTRY_MASTER   (identity_node_id() == REGISTRY_MASTER_NODE)

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
    uint8_t retries;
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
    m.len     = (uint8_t)len;
    m.retain  = retain;
    m.retries = 0;
    return xQueueSend(s_pub_q, &m, 0) == pdTRUE;
}

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

/* ------------------------------------------------------------ receive loop */

static uint32_t s_rx_us;
uint32_t net_rx_time_us(void) { return s_rx_us; }

static bool dispatch_to_handlers(const mqtt_message_t *msg) {
    bool handled = false;
    s_rx_us = msg->rx_us;
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
    uint8_t sent = 0;
    while (sent < 3 && xQueuePeek(s_pub_q, &m, 0) == pdTRUE) {
        if (mqtt_send_ex(m.topic, m.data, m.len, 0, m.retain, NULL, NULL)) {
            xQueueReceive(s_pub_q, &m, 0);
            sent++;
        } else {
            xQueueReceive(s_pub_q, &m, 0);
            if (++m.retries < 5) {
                xQueueSendToFront(s_pub_q, &m, 0);
            } else {
                printf("Net: publish to %s dropped after retries.\n", m.topic);
            }
            break;
        }
    }
}

/* ------------------------------------------------------------ sensor tx */

static void send_movement(app_event_t evt) {
    static uint32_t last_send_ms = 0;

    if (!is_wifi_connected() || !is_mqtt_connected()) return;

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (last_send_ms != 0 && (now_ms - last_send_ms) < 450) return;

    uint8_t tap_val = 0;
    if (evt == APP_EVT_MOTION_SINGLE)      tap_val = 1;
    else if (evt == APP_EVT_MOTION_DOUBLE) tap_val = 2;
    else if (evt == APP_EVT_MOTION_TRIPLE) tap_val = 3;
    else return;

    uint8_t payload = (uint8_t)((identity_node_id() & 0x0F) | ((tap_val & 0x03) << 6));
    if (net_publish(MQTT_TAP_TOPIC, &payload, 1, false)) {
        last_send_ms = now_ms;
        printf("[%lu ms] Movement: tap type %u sent to Game Master\n",
               (unsigned long)now_ms, (unsigned)tap_val);
    }
}

static void handle_sensor_events(void) {
    app_event_t evt;
    while (xQueueReceive(net_event_q, &evt, 0) == pdTRUE) {
        if (evt == APP_EVT_MOTION_SINGLE ||
            evt == APP_EVT_MOTION_DOUBLE ||
            evt == APP_EVT_MOTION_TRIPLE) {
            send_movement(evt);
        }
    }
}

/* ---------------------------------------------- join / registry */

static bool is_board_id_text(const char *s) {
    if (strlen(s) != IDENTITY_BOARD_ID_LEN - 1) return false;
    for (size_t i = 0; s[i]; ++i) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

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
    }
}

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

        if (mqtt_is_ready()) {
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
                identity_set_node_id(id);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("Assigned panel %d\n", identity_node_id());
    mqtt_reset_connection();
}

/* ------------------------------------------------------------ command dispatch */
static void execute_node_command(const char *buf, size_t len) {
    typedef struct {
        const char    *name;
        uint8_t        name_len;
        const uint8_t *sprite;
    } sprite_cmd_t;

    static const sprite_cmd_t sprite_cmds[] = {
        { "diamond", 7, diamondCard_bin },
        { "Clover",  6, cloverCard_bin  },
        { "spade",   5, spadeCard_bin   },
        { "win",     3, winSTAR_bin     },
        { "lose",    4, loseREJECT_bin  }
    };

    if (len == 3 && memcmp(buf, "OFF", 3) == 0) {
        display_show_colour(0, 0, 0);
        return;
    }

    if (len == 4 && memcmp(buf, "stop", 4) == 0) {
        buzzer_stop();
        return;
    }

    for (size_t i = 0; i < sizeof(sprite_cmds) / sizeof(sprite_cmds[0]); i++) {
        if (len == sprite_cmds[i].name_len &&
            memcmp(buf, sprite_cmds[i].name, sprite_cmds[i].name_len) == 0) {
            display_show_image(sprite_cmds[i].sprite);
            return;
        }
    }

    if (len == 3 && memcmp(buf, "dj", 2) == 0 && buf[2] >= '1' && buf[2] <= '6') {
        static const buzz_id_t dj_sounds[6] = {
            BUZZ_DJ_1, BUZZ_DJ_2, BUZZ_DJ_3, BUZZ_DJ_4, BUZZ_DJ_5, BUZZ_DJ_6
        };
        buzzer_play(dj_sounds[buf[2] - '1']);
        return;
    }

    if (len == 5 && memcmp(buf, "fart", 4) == 0 && buf[4] >= '1' && buf[4] <= '6') {
        static const buzz_id_t fart_sounds[6] = {
            BUZZ_FART_1, BUZZ_FART_2, BUZZ_FART_3, BUZZ_FART_4, BUZZ_FART_5, BUZZ_FART_6
        };
        buzzer_play(fart_sounds[buf[4] - '1']);
        return;
    }

    unsigned a = 0, b = 0, kit = 0;
    unsigned long total_ms = 30000, delay_ms = 0;
    char dir = 'R', extra = '\0';

    if (sscanf(buf, "burst:%u:%u%c", &a, &kit, &extra) == 2 && a >= 1 && a <= NUM_PANELS && kit <= 1) {
        display_anim_burst((uint8_t)a, (uint8_t)kit);
    } else if (sscanf(buf, "tx_out:%u:%u:%c%c", &a, &kit, &dir, &extra) == 3 &&
               a >= 1 && a <= NUM_PANELS && kit <= 1 && (dir == 'R' || dir == 'L')) {
        display_anim_tx_out((uint8_t)a, (uint8_t)kit, dir == 'R');
    } else if (sscanf(buf, "tx_pass:%u:%u:%c%c", &a, &kit, &dir, &extra) == 3 &&
               a >= 1 && a <= NUM_PANELS && kit <= 1 && (dir == 'R' || dir == 'L')) {
        display_anim_tx_pass((uint8_t)a, (uint8_t)kit, dir == 'R');
    } else if (sscanf(buf, "tx_in:%u:%u:%u:%c%c", &a, &b, &kit, &dir, &extra) == 4 &&
               a >= 1 && a <= NUM_PANELS && b >= 1 && b <= NUM_PANELS &&
               kit <= 1 && (dir == 'R' || dir == 'L')) {
        display_anim_tx_in((uint8_t)a, (uint8_t)b, (uint8_t)kit, dir == 'R');
    } else if (sscanf(buf, "mario_all:%u:%u:%lu%c", &a, &b, &total_ms, &extra) == 3 &&
               b >= 1 && b <= NUM_PANELS && a < b) {
        display_anim_mario((uint8_t)a, (uint8_t)b, (uint32_t)total_ms);
    } else if (sscanf(buf, "start_all:%u:%u:%u%c", &a, &b, &kit, &extra) == 3 &&
               b >= 1 && b <= NUM_PANELS && a < b) {
        display_anim_start_word((uint8_t)a, (uint8_t)b, (uint16_t)kit);
    } else if (sscanf(buf, "ms:%u:%u:%lu%c", &a, &b, &delay_ms, &extra) == 3 &&
               a < b && b <= 255) {
        buzzer_play_mario_slice_delayed((uint8_t)a, (uint8_t)b, (uint32_t)delay_ms);
    } else if (sscanf(buf, "ms:%u:%u%c", &a, &b, &extra) == 2 &&
               a < b && b <= 255) {
        buzzer_play_mario_slice_delayed((uint8_t)a, (uint8_t)b, 0);
    } else {
        printf("Net: unrecognised command '%s'\n", buf);
    }
}

static void apply_received_command(void) {
    mqtt_message_t message;
    for (size_t i = 0; i < MQTT_RX_QUEUE_CAPACITY && mqtt_receive(&message); ++i) {
        if (IS_REGISTRY_MASTER && strcmp(message.topic, MQTT_JOIN_TOPIC) == 0) {
            handle_join_request(&message);
            continue;
        }
        if (dispatch_to_handlers(&message)) continue;
        if (strcmp(message.topic, identity_command_topic()) != 0) continue;

        char buf[32];
        size_t n = (message.length < sizeof(buf) - 1) ? message.length : (sizeof(buf) - 1);
        memcpy(buf, message.payload, n);
        buf[n] = '\0';
        execute_node_command(buf, n);
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
        if (mqtt_send_ex(identity_status_topic(), MQTT_STATUS_MESSAGE,
                         strlen(MQTT_STATUS_MESSAGE), 0, true, NULL, NULL)) {
            next_publish = now + pdMS_TO_TICKS(2500);
        } else {
            next_publish = now + pdMS_TO_TICKS(500);
        }
    }
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

    if (!identity_is_assigned()) join_grid(client_id);
    if (IS_REGISTRY_MASTER) registry_init();

    if (!mqtt_configure_will(identity_status_topic(), "offline", 10) ||
        !mqtt_subscribe_topic(identity_command_topic()) ||
        (IS_REGISTRY_MASTER && !mqtt_subscribe_topic(MQTT_JOIN_TOPIC))) {
        printf("MQTT topic registration failed.\n");
        vTaskDelete(NULL);
    }

    for (size_t i = 0; i < s_handler_count; i++) {
        if (!mqtt_subscribe_topic(s_handlers[i].filter)) {
            printf("Could not register %s.\n", s_handlers[i].filter);
            vTaskDelete(NULL);
        }
    }

    bool display_started = false;
    bool wifi_was_connected = true;
    bool mqtt_was_ready = false;
    TickType_t next_wifi_retry = xTaskGetTickCount();

    for (;;) {
        bool wifi_connected = is_wifi_connected();
        if (wifi_connected != wifi_was_connected) {
            printf("Wi-Fi %s.\n", wifi_connected ? "connected" : "disconnected");
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
                wifi_init_and_connect(WIFI_SSID, WIFI_PASSWORD);
                next_wifi_retry = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
                xQueueReset(net_event_q);
                xQueueReset(s_pub_q);
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
            publish_status();
            clock_sync_poll();
            drain_publish_queue();
        } else {
            xQueueReset(net_event_q);
            xQueueReset(s_pub_q);
        }

        mqtt_was_ready = mqtt_ready;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool net_start(void) {
    net_event_q = sensor_get_event_queue();
    s_pub_q = xQueueCreate(NET_PUB_QUEUE_LEN, sizeof(net_pub_t));
    if (!net_event_q || !s_pub_q) return false;
    return xTaskCreate(net_task, "net", NET_TASK_STACK, NULL, NET_TASK_PRIORITY, NULL) == pdPASS;
}