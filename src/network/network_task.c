#include "network_task.h"
#include "task.h"
#include "event_groups.h"
#include "wifi_manager.h"
#include <string.h>
#include <stdio.h>

#define READY_BIT (1u << 0)
#define QUEUE_LENGTH 8
typedef struct {
    mqtt_message_t message;
    uint8_t qos;
    bool retain;
} publish_request_t;
static QueueHandle_t tx_queue, rx_queue;
static EventGroupHandle_t state;
static uint32_t drops;

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
    return state && (xEventGroupGetBits(state) & READY_BIT);
}
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
    return xQueueSend(tx_queue, &request, 0) == pdPASS;
}
bool network_receive(mqtt_message_t *message, TickType_t wait) {
    return rx_queue && message && xQueueReceive(rx_queue, message, wait) == pdPASS;
}
static void discard_pending(void) {
    publish_request_t request;
    mqtt_message_t message;
    while (xQueueReceive(tx_queue, &request, 0) == pdPASS) drop();
    while (xQueueReceive(rx_queue, &message, 0) == pdPASS) drop();
}
static void network_task(void *arg) {
    const network_task_config_t *config = arg;
    // Wi-Fi initialisation must happen inside a running task for sys_freertos.
    while (!wifi_init_and_connect(config->ssid, config->password))
        vTaskDelay(pdMS_TO_TICKS(5000));
    configASSERT(mqtt_configure_will(config->status_topic, "offline", 10));
    configASSERT(mqtt_subscribe_topic_qos(config->subscription, 1));
    TickType_t last_status = 0;
    bool was_ready = false;
    for (;;) {
        bool wifi = is_wifi_connected();
        if (wifi) init_mqtt_client(config->client_id);
        bool ready = wifi && mqtt_is_ready();
        if (!ready) {
            xEventGroupClearBits(state, READY_BIT);
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
            xEventGroupSetBits(state, READY_BIT);
            TickType_t now = xTaskGetTickCount();
            if (!was_ready || now - last_status >= pdMS_TO_TICKS(1000)) {
                if (mqtt_send_ex(config->status_topic, "online", 6, 1, true, NULL, NULL))
                    last_status = now;
            }
            was_ready = true;
            publish_request_t request;
            for (unsigned i = 0; i < QUEUE_LENGTH &&
                 xQueueReceive(tx_queue, &request, 0) == pdPASS; ++i) {
                if (!mqtt_send_ex(request.message.topic, request.message.payload,
                                  request.message.length, request.qos, request.retain,
                                  NULL, NULL)) drop();
            }
            mqtt_message_t message;
            for (unsigned i = 0; i < MQTT_RX_QUEUE_CAPACITY && mqtt_receive(&message); ++i)
                if (xQueueSend(rx_queue, &message, 0) != pdPASS) drop();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
bool network_task_start(const network_task_config_t *config) {
    if (state || !config || !config->ssid || !config->password || !config->client_id ||
        !config->subscription || !config->status_topic) return false;
    tx_queue = xQueueCreate(QUEUE_LENGTH, sizeof(publish_request_t));
    rx_queue = xQueueCreate(QUEUE_LENGTH, sizeof(mqtt_message_t));
    state = xEventGroupCreate();
    if (tx_queue && rx_queue && state &&
        xTaskCreate(network_task, "Network", 2048, (void *)config, 3, NULL) == pdPASS)
        return true;
    if (tx_queue) vQueueDelete(tx_queue);
    if (rx_queue) vQueueDelete(rx_queue);
    if (state) vEventGroupDelete(state);
    tx_queue = rx_queue = NULL;
    state = NULL;
    return false;
}
