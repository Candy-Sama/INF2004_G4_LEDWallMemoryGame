#include "network_task.h"
#include "task.h"
#include "queue.h"
#include "wifi_manager.h"
#include <string.h>
#include <stdio.h>

#define QUEUE_LENGTH 8
#define NETWORK_PRIORITY 3
#define NETWORK_STACK_WORDS 2048
#define SERVICE_INTERVAL_MS 10
#define STATUS_INTERVAL_MS 1000
typedef struct {
    mqtt_message_t message;
    uint8_t qos;
    bool retain;
} publish_request_t;
// TX: application -> network owner. RX: network owner -> application.
// Queues copy complete messages; callers can reuse their buffers after sending.
static QueueHandle_t tx_queue, rx_queue;
static bool ready_for_messages;
static uint32_t drops;

// Shared diagnostics: a critical section prevents concurrent reads/updates.
static void drop(void) {
    taskENTER_CRITICAL();
    ++drops;
    taskEXIT_CRITICAL();
}
uint32_t network_drop_count(void) {
    taskENTER_CRITICAL();
    uint32_t value = drops;
    taskEXIT_CRITICAL();
    return value;
}
bool network_ready(void) {
    taskENTER_CRITICAL();
    bool ready = ready_for_messages;
    taskEXIT_CRITICAL();
    return ready;
}
static void set_ready(bool ready) {
    taskENTER_CRITICAL();
    ready_for_messages = ready;
    taskEXIT_CRITICAL();
}

// Called by application tasks. Validate and copy; never access lwIP here.
bool network_publish(const char *topic, const void *payload, size_t length,
                     uint8_t qos, bool retain) {
    if (!network_ready() || !topic || !topic[0] ||
        strlen(topic) > MQTT_TOPIC_MAX_LENGTH || strpbrk(topic, "+#") ||
        length > MQTT_PAYLOAD_MAX_LENGTH || (length && !payload) || qos > 1) return false;
    publish_request_t request = {0};
    strcpy(request.message.topic, topic);
    request.message.length = length;
    if (length) memcpy(request.message.payload, payload, length);
    request.qos = qos;
    request.retain = retain;
    // Zero wait: a full queue must not stall sensor/game work.
    return xQueueSend(tx_queue, &request, 0) == pdPASS;
}
bool network_receive(mqtt_message_t *message, TickType_t wait_ticks) {
    // message points to caller-owned storage where FreeRTOS copies the result.
    // Only this calling task blocks when the queue is empty, not the whole Pico.
    return rx_queue && message && xQueueReceive(rx_queue, message, wait_ticks) == pdPASS;
}
static void discard_pending(void) {
    publish_request_t request;
    // Reuse the same storage for both queue types; RX needs only its message.
    while (xQueueReceive(tx_queue, &request, 0) == pdPASS) drop();
    while (xQueueReceive(rx_queue, &request.message, 0) == pdPASS) drop();
}
// Bound both batches so busy traffic cannot monopolise the Network Task.
// Reuse one request buffer for TX and RX to keep stack use modest.
static void transfer_messages(void) {
    publish_request_t request;
    for (unsigned i = 0; i < QUEUE_LENGTH &&
         xQueueReceive(tx_queue, &request, 0) == pdPASS; ++i) {
        if (!mqtt_send_ex(request.message.topic, request.message.payload,
                          request.message.length, request.qos, request.retain,
                          NULL, NULL)) drop();
    }
    for (unsigned i = 0; i < MQTT_RX_QUEUE_CAPACITY && mqtt_receive(&request.message); ++i)
        if (xQueueSend(rx_queue, &request.message, 0) != pdPASS) drop();
}

static void network_task(void *arg) {
    const network_task_config_t *config = arg;
    // Wi-Fi initialisation must happen inside a running task for sys_freertos.
    while (!wifi_init_and_connect(config->ssid, config->password))
        vTaskDelay(pdMS_TO_TICKS(5000));
    // These must succeed before any MQTT connection attempt.
    configASSERT(mqtt_configure_will(config->status_topic, "offline", 10));
    configASSERT(mqtt_subscribe_topic_qos(config->subscription, 1));
    TickType_t last_status = 0;
    bool was_ready = false;
    for (;;) {
        bool wifi = is_wifi_connected();
        if (wifi) init_mqtt_client(config->client_id);
        bool ready = wifi && mqtt_is_ready();
        if (!ready) {
            set_ready(false);
            discard_pending();
            if (was_ready) printf("Network unavailable; pending application messages discarded.\n");
            was_ready = false;
            if (!wifi) {
                // Retry connection from the same owner; SDK waits block this task,
                // not the sensor/display tasks or lwIP service task.
                wifi_init_and_connect(config->ssid, config->password);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            set_ready(true);
            // Retrying presence periodically also retries a locally failed send.
            TickType_t now = xTaskGetTickCount();
            if (!was_ready || now - last_status >= pdMS_TO_TICKS(STATUS_INTERVAL_MS)) {
                if (mqtt_send_ex(config->status_topic, "online", 6, 1, true, NULL, NULL))
                    last_status = now;
            }
            was_ready = true;
            transfer_messages();
        }
        // Block instead of spinning: other ready tasks get CPU time.
        vTaskDelay(pdMS_TO_TICKS(SERVICE_INTERVAL_MS));
    }
}
bool network_task_start(const network_task_config_t *config) {
    if (tx_queue || !config || !config->ssid || !config->password || !config->client_id ||
        !config->subscription || !config->status_topic) return false;
    tx_queue = xQueueCreate(QUEUE_LENGTH, sizeof(publish_request_t));
    rx_queue = xQueueCreate(QUEUE_LENGTH, sizeof(mqtt_message_t));
    if (tx_queue && rx_queue &&
        xTaskCreate(network_task, "Network", NETWORK_STACK_WORDS, (void *)config, NETWORK_PRIORITY, NULL) == pdPASS)
        return true;
    // Startup failed: release partial allocations so another attempt is safe.
    if (tx_queue) vQueueDelete(tx_queue);
    if (rx_queue) vQueueDelete(rx_queue);
    tx_queue = rx_queue = NULL;
    return false;
}
