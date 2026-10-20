#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "network_task.h"
#include "queue.h"

struct test_queue { unsigned size, capacity, head, count; unsigned char *data; };
static TickType_t observed_wait;
QueueHandle_t xQueueCreate(unsigned count, unsigned size) {
    QueueHandle_t q = calloc(1, sizeof(*q));
    assert(q);
    q->data = calloc(count, size); assert(q->data);
    q->capacity = count; q->size = size; return q;
}
int xQueueSend(QueueHandle_t q, const void *item, TickType_t wait) {
    (void)wait;
    if (q->count == q->capacity) return 0;
    memcpy(q->data + ((q->head + q->count) % q->capacity) * q->size, item, q->size);
    ++q->count; return pdPASS;
}
int xQueueReceive(QueueHandle_t q, void *item, TickType_t wait) {
    observed_wait = wait;
    if (!q->count) return 0;
    memcpy(item, q->data + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->capacity; --q->count; return pdPASS;
}
void vQueueDelete(QueueHandle_t q) { free(q->data); free(q); }
static void (*task_entry)(void *);
static void *task_arg;
static jmp_buf iteration;
static bool ready, incoming, send_ok = true;
static int sends;
static char received_payload[16];
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, TaskHandle_t *handle) {
    (void)name; (void)stack; (void)priority; (void)handle;
    task_entry = entry; task_arg = arg; return pdPASS;
}
void vTaskDelay(TickType_t ticks) { assert(ticks > 0); longjmp(iteration, 1); }
TickType_t xTaskGetTickCount(void) { return 1000; }
bool wifi_init_and_connect(const char *s, const char *p) { return s && p; }
bool is_wifi_connected(void) { return true; }
bool mqtt_configure_will(const char *t, const char *p, uint16_t k) { return t && p && k; }
bool mqtt_subscribe_topic_qos(const char *t, uint8_t q) { return t && q == 1; }
void init_mqtt_client(const char *id) { assert(id); }
bool mqtt_is_ready(void) { return ready; }
bool mqtt_send_ex(const char *topic, const void *payload, size_t length,
                  uint8_t qos, bool retain, mqtt_send_completion_t cb, void *arg) {
    (void)qos; (void)retain; (void)cb; (void)arg;
    if (!strcmp(topic, "tap")) {
        ++sends;
        assert(length < sizeof(received_payload));
        memcpy(received_payload, payload, length); received_payload[length] = 0;
    }
    return send_ok;
}
bool mqtt_receive(mqtt_message_t *m) {
    if (!incoming) return false;
    *m = (mqtt_message_t){.topic = "state", .payload = {'R', 0, 'D'}, .length = 3};
    incoming = false; return true;
}
static void step(void) {
    // Stops at the task's first blocking delay. This tests queue routing, not
    // FreeRTOS scheduling. SDK/FreeRTOS are exercised by the firmware build.
    if (!setjmp(iteration)) task_entry(task_arg);
}
int main(void) {
    network_task_config_t config = {"ssid", "password", "id", "command", "status"};
    assert(!network_publish("tap", "x", 1, 1, false));
    assert(!network_task_start(NULL));
    assert(network_task_start(&config));
    assert(!network_task_start(&config));
    ready = true; step(); assert(network_ready());
    char bytes[] = "TAP";
    assert(network_publish("tap", bytes, 3, 1, false));
    bytes[0] = 'X';
    step(); assert(sends == 1 && !strcmp(received_payload, "TAP"));
    assert(!network_publish("bad/+", "x", 1, 1, false));
    assert(!network_publish("tap", NULL, 1, 1, false));
    assert(!network_publish("tap", "x", 1, 2, false));
    for (int i = 0; i < 8; ++i) assert(network_publish("tap", "x", 1, 1, false));
    assert(!network_publish("tap", "x", 1, 1, false));
    ready = false; step(); assert(!network_ready());
    assert(network_drop_count() == 8);
    ready = true; incoming = true; step();
    mqtt_message_t m;
    assert(network_receive(&m, 0) && m.length == 3 && m.payload[1] == 0);
    assert(!network_receive(&m, 0));
    assert(!network_receive(&m, pdMS_TO_TICKS(20)) && observed_wait == 20);
    assert(!network_receive(NULL, 0));
    assert(network_publish("tap", "x", 1, 1, false));
    send_ok = false; step(); assert(network_drop_count() == 9);
    for (int i = 0; i < 9; ++i) { incoming = true; step(); }
    assert(network_drop_count() == 10); // Ninth incoming message exceeds app FIFO.
    ready = false; step();
    assert(network_drop_count() == 18); // Eight old incoming messages discarded.
    assert(!network_receive(&m, 0));
    puts("Network task queue tests passed.");
}
