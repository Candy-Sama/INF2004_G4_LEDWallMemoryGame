#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/i2c.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "node_config.h"
#include "wifi_manager.h"
#include "wifi_creds.h" // Standard build: application supplies configured credentials.
#include "mqtt_handler.h"
#include "imudriver.h"
#include "motion_detector.h"
#include "ledmatrix.h"
#include "imagedata.h"
#include "game_logic.h"
#include "pico/rand.h"

#ifndef WAIT_FOR_USB
#define WAIT_FOR_USB 0
#endif

#define I2C_PORT i2c0
#define I2C_SDA 0
#define I2C_SCL 1

// Task priorities (higher number = more urgent). The Wi-Fi driver task (4) and
// lwIP's tcpip thread (5) sit above these.
#define SENSOR_TASK_PRIORITY   3
#define NET_TASK_PRIORITY      2
#define DISPLAY_TASK_PRIORITY  2
 
// Stack sizes are in 32-bit words, not bytes.
#define SENSOR_TASK_STACK   1024
#define NET_TASK_STACK      2048
#define DISPLAY_TASK_STACK  512

// Pick your Master A id
#define IS_GAME_MASTER (NODE_ID == 1)
#define GAME_TASK_PRIORITY   3
#define GAME_TASK_STACK      1024
#define GAME_TEST_FORCE_ONLINE 1

 
// typedef struct { uint8_t r, g, b; } display_cmd_t;
typedef struct {
    const uint8_t *image; //NULL means fill with R,G,B
    uint8_t r,g,b;
} display_cmd_t;

typedef enum { APP_EVT_MOTION } app_event_t;

typedef struct { uint8_t bytes[4]; uint8_t len; uint8_t kind; } net_out_t;
enum { OUT_STATE, OUT_REVEAL };

static QueueHandle_t net_event_q;   // sensor_task -> net_task
static QueueHandle_t display_q;     // net_task    -> display_task

static GameShared game;
static QueueHandle_t tap_q;         //net_task > game_task (tap_vt_t)
static QueueHandle_t net_out_q;     //game_task > net_task (net_out_t)

static void show_colour(uint8_t r, uint8_t g, uint8_t b) {
    display_cmd_t cmd = {NULL, r, g, b };
    // Never block the caller; if the panel is busy the newest colour is dropped.
    xQueueSend(display_q, &cmd, 0);
}

static void show_image(const uint8_t *rgb){
    display_cmd_t cmd = {rgb, 0, 0, 0};
    // Never block the caller; if the panel is busy the newest colour is dropped.
    xQueueSend(display_q, &cmd, 0);
}


static bool setup_sensor(void) {
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    printf("IMU: SDA=GP%d, SCL=GP%d\n", I2C_SDA, I2C_SCL);
    return imu_init(I2C_PORT);
}

static void sensor_task(void *arg) {
    (void)arg;
    if (!setup_sensor()) {
        printf("IMU not available; sensor task stopping.\n");
        vTaskDelete(NULL);
    }
 
    motion_detector_t detector;
    memset(&detector, 0, sizeof(detector));
#if IMU_LOG_READINGS
    TickType_t next_print = xTaskGetTickCount();
#endif
 
    for (;;) {
        imu_acceleration_t sample;
        imu_read_result_t result = imu_read_acceleration(&sample);
 
        if (result == IMU_READ_ERROR) {
            detector.initialised = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (result == IMU_NEW_DATA) {
#if IMU_LOG_READINGS
            if ((int32_t)(xTaskGetTickCount() - next_print) >= 0) {
                printf("Node %d IMU: X=%d mg Y=%d mg Z=%d mg\n",
                       NODE_ID, sample.x_mg, sample.y_mg, sample.z_mg);
                next_print = xTaskGetTickCount() + pdMS_TO_TICKS(500);
            }
#endif
            bool moved = motion_detector_update(&detector, sample.x_mg, sample.y_mg,
                                                sample.z_mg,
                                                to_ms_since_boot(get_absolute_time()));
            // Only node 1 publishes movement commands.
            if (moved && ENABLE_MOTION_TEST && NODE_ID == 1) {
                app_event_t evt = APP_EVT_MOTION;
                xQueueSend(net_event_q, &evt, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

//game task only on game master
 static void game_task(void *arg) {
    (void)arg;
    tap_evt_t evt;
    for (;;) {
        if (xQueueReceive(tap_q, &evt, pdMS_TO_TICKS(250)) == pdTRUE) {
            game_shared_process_input(&game, evt.panel_id);
        }

        GameManager snap;
        if (!game_shared_snapshot(&game, &snap)) continue;

        net_out_t out = { .kind = OUT_STATE, .len = 2 };
        game_pack_state(&snap, out.bytes);
        xQueueSend(net_out_q, &out, 0);

        if (snap.current_state == STATE_EVALUATE_MATCH) {
            net_out_t rev = { .kind = OUT_REVEAL, .len = 1 };
            rev.bytes[0] = (snap.first_selection & 7) | ((snap.second_selection & 7) << 3);
            xQueueSend(net_out_q, &rev, 0);

            vTaskDelay(pdMS_TO_TICKS(1000));      // lock NOT held
            game_shared_evaluate(&game);
            xQueueReset(tap_q);
        }
    }
}

/* --------------------------------------------------------------- network */

 
// Application-specific interpretation stays out of the MQTT transport.


// Runs in the main loop, not in a network callback.
/* static bool read_movement(void) {
    static absolute_time_t next_read;
    static motion_detector_t detector;
#if IMU_LOG_READINGS
    static absolute_time_t next_print;
#endif
    if (!time_reached(next_read)) {
        return false;
    }
    next_read = make_timeout_time_ms(10);

    imu_acceleration_t sample;
    imu_read_result_t result = imu_read_acceleration(&sample);
    if (result == IMU_READ_ERROR) {
        detector.initialised = false;
        next_read = make_timeout_time_ms(500);
        return false;
    }
    if (result != IMU_NEW_DATA) {
        return false;
    }
#if IMU_LOG_READINGS
    if (time_reached(next_print)) {
        printf("Node %d IMU: X=%d mg Y=%d mg Z=%d mg\n",
               NODE_ID, sample.x_mg, sample.y_mg, sample.z_mg);
        next_print = make_timeout_time_ms(500);
    }
#endif
    return motion_detector_update(&detector, sample.x_mg, sample.y_mg,
                                  sample.z_mg, to_ms_since_boot(get_absolute_time()));
} */

static void send_movement(void) {
    // static bool send_green_next = true;
    static bool send_heart_next = true;
    if (!is_wifi_connected() || !is_mqtt_connected()) {
        printf("Movement ignored: network is disconnected.\n");
        return;
    }
    const char *card = send_heart_next ? "Heart" : "Dark Heart";
    if (mqtt_send(MQTT_PEER_COMMAND_TOPIC, card, strlen(card))) {
        printf("Movement: queued %s to %s\n", card, MQTT_PEER_COMMAND_TOPIC);
        show_image(send_heart_next ? heartCard_bin : heartCardDark_bin);
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
        // tap from a slave receiving on master only.
        if (IS_GAME_MASTER && strcmp(message.topic, MQTT_TAP_TOPIC) == 0) {
            if (message.length == 1) {
                uint8_t b = ((const uint8_t *)message.payload)[0];
                tap_evt_t evt = { .panel_id = b & 7, .from_node = (b >> 3) & 7 };
                xQueueSend(tap_q, &evt, 0);
            }
            continue;
        }
        if (strcmp(message.topic, MQTT_COMMAND_TOPIC) != 0) continue;
        if (message.length == 5 && memcmp(message.payload, "Heart", 5) == 0) {
            show_image(heartCard_bin);
            printf("LED command applied: Heart\n");
        } else if (message.length == 10 && memcmp(message.payload, "Dark Heart", 10) == 0) {
            show_image(heartCardDark_bin);
            printf("LED command applied: Dark Heart\n");
        } else if (message.length == 3 && memcmp(message.payload, "OFF", 3) == 0) {
            ledmatrix_fill(0, 0, 0);
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
        mqtt_send_ex(MQTT_PRESENCE_TOPIC, MQTT_STATUS_MESSAGE,
                     strlen(MQTT_STATUS_MESSAGE), 1, true, NULL, NULL);
        next_publish = now + pdMS_TO_TICKS(1000);
    }
}

// drains game_task output and publishes it
static void publish_game_output(void) {
    net_out_t out;
    while (xQueueReceive(net_out_q, &out, 0) == pdTRUE) {
        const char *topic = (out.kind == OUT_STATE) ? MQTT_STATE_TOPIC
                                                    : MQTT_REVEAL_TOPIC;
        mqtt_send_ex(topic, (const char *)out.bytes, out.len, 1,
                     out.kind == OUT_STATE, NULL, NULL);
    }
}

static void net_task(void *arg) {
    (void)arg;
 
    // Client IDs identify physical boards; NODE_ID chooses their MQTT topics.
    char board_id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    static char client_id[32];   // must stay valid for the lifetime of the connection
    pico_get_unique_board_id_string(board_id, sizeof(board_id));
    snprintf(client_id, sizeof(client_id), "SE4_%s", board_id);
    printf("MQTT client: %s\nListen: %s\n", client_id, MQTT_COMMAND_TOPIC);
 
    // Wi-Fi setup must run inside a task when using the FreeRTOS cyw43 arch.
    while (!wifi_init_and_connect(WIFI_SSID, WIFI_PASSWORD)) {
        printf("Wi-Fi setup failed. Retrying in 5 s...\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
 
    // Broker publishes retained 'offline' if this connection disappears.
    if (!mqtt_configure_will(MQTT_PRESENCE_TOPIC, "offline", 10)) {
        printf("Could not configure MQTT Last Will.\n");
        vTaskDelete(NULL);
    }
    if (!mqtt_subscribe_topic(MQTT_COMMAND_TOPIC)) {
        printf("Could not register the command topic.\n");
        vTaskDelete(NULL);
    }

    //master also listens for taps
    if (IS_GAME_MASTER && !mqtt_subscribe_topic(MQTT_TAP_TOPIC)) {
        printf("Could not register the tap topic.\n");
        vTaskDelete(NULL);
    }
 
    bool display_started = false;
    bool wifi_was_connected = true;
 
    for (;;) {
        bool wifi_connected = is_wifi_connected();
        if (wifi_connected != wifi_was_connected) {
            printf("Wi-Fi %s.\n", wifi_connected ? "available" : "lost; restore network or restart");
            wifi_was_connected = wifi_connected;
        }
 
        // Always drain sensor events so stale movements are never replayed later.
        handle_sensor_events();
 
        if (wifi_connected) {
            // Keep servicing even after connecting: subscriptions may need retries.
            init_mqtt_client(client_id);
            if (mqtt_is_ready()) {
                if (!display_started) {
                    show_image(Number1CardDark_bin);   // default number card - hardcodded to 1 for testing
                    display_started = true;
                }
                apply_received_command();
                publish_status();
                if (IS_GAME_MASTER) publish_game_output(); 
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
 
/* --------------------------------------------------------------- display */
 
static void display_task(void *arg) {
    (void)arg;
    ledmatrix_init();
     display_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(display_q, &cmd, portMAX_DELAY) == pdTRUE) {
            if (cmd.image){
                show_image_rgb(cmd.image);
            }else{
            ledmatrix_fill(cmd.r, cmd.g, cmd.b);
        }
        }
    }
}
 
/* ------------------------------------------------------- FreeRTOS hooks */
 
void vApplicationMallocFailedHook(void) {
    printf("FATAL: FreeRTOS heap exhausted (raise configTOTAL_HEAP_SIZE).\n");
    for (;;) {}
}
 
void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    printf("FATAL: stack overflow in task '%s' (raise its stack size).\n", name);
    for (;;) {}
}
 
/* ------------------------------------------------------------------ main */
 
int main(void) {
    stdio_init_all();
#if WAIT_FOR_USB
    // Open the USB serial monitor to start this teaching/demo firmware.
    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    sleep_ms(1000);
#endif
    printf("\n--- NODE %d: FREERTOS SENSOR / MQTT / LED DEMO ---\n", NODE_ID);

    srand(get_rand_32());
 
    net_event_q = xQueueCreate(8, sizeof(app_event_t));
    display_q   = xQueueCreate(4, sizeof(display_cmd_t));
    tap_q       = xQueueCreate(8, sizeof(tap_evt_t));
    net_out_q   = xQueueCreate(8, sizeof(net_out_t));
    if (net_event_q == NULL || display_q == NULL || !tap_q || !net_out_q) {
        printf("Could not create queues.\n");
        for (;;) {}
    }

    if (IS_GAME_MASTER) {
        if (!game_shared_init(&game)) {
            printf("Game init failed.\n");
            for (;;) {}
        }
    
#if GAME_TEST_FORCE_ONLINE
    for (uint8_t n = 0; n < NUM_PANELS; n++) game_shared_set_online(&game, n, true);
#endif
        xTaskCreate(game_task, "game", GAME_TASK_STACK, NULL, GAME_TASK_PRIORITY, NULL);
    }
 
    xTaskCreate(sensor_task,  "sensor",  SENSOR_TASK_STACK,  NULL, SENSOR_TASK_PRIORITY,  NULL);
    xTaskCreate(net_task,     "net",     NET_TASK_STACK,     NULL, NET_TASK_PRIORITY,     NULL);
    xTaskCreate(display_task, "display", DISPLAY_TASK_STACK, NULL, DISPLAY_TASK_PRIORITY, NULL);
 
    vTaskStartScheduler();
    for (;;) {}   // only reached if the scheduler could not start (out of heap)
}
