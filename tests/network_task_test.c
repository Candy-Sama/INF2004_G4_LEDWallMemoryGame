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
static int sends, stage, ack_sends;
static bool test_recovery;
static uint32_t clock_ms = 1000, generation = 1, lower_drops;
static uint8_t token[8], previous_token[8];
static mqtt_message_t injected;
static void check_iteration(void);

uint32_t mqtt_received_drop_count(void) { return lower_drops; }
uint32_t mqtt_connection_generation(void) { return generation; }
uint64_t get_absolute_time(void) { return clock_ms; }
uint32_t to_ms_since_boot(uint64_t time) { return (uint32_t)time; }

static char received_payload[16];
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, TaskHandle_t *handle) {
    (void)name; (void)stack; (void)priority; (void)handle;
    task_entry = entry; task_arg = arg; return pdPASS;
}
void vTaskDelay(TickType_t ticks) { assert(ticks > 0); clock_ms += ticks; check_iteration(); }
TickType_t xTaskGetTickCount(void) { return clock_ms; }
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
    if (!strcmp(topic, "sync")) { assert(length == 8); memcpy(token, payload, 8); }
    if (!strcmp(topic, "applied")) { assert(length == 12); ++ack_sends; }
    return send_ok;
}
bool mqtt_receive(mqtt_message_t *m) {
    if (!incoming) return false;
    *m = test_recovery ? injected : (mqtt_message_t){.topic = "state", .payload = {'R', 0, 'D'}, .length = 3};
    incoming = false; return true;
}

static void inject_recovery(const char *topic, bool old) {
    injected = (mqtt_message_t){0};
    strcpy(injected.topic, topic);
    memcpy(injected.payload, old ? previous_token : token, 8);
    injected.payload[11] = 7;
    injected.payload[12] = 2;
    injected.length = !strcmp(topic, "snapshot") ? 13 : 12;
    incoming = true;
}
static void check_iteration(void) {
    mqtt_message_t m;
    int n = stage++;
    if (test_recovery) {
        switch (n) {
        case 0:
            assert(network_ready() && !network_game_ready());
            assert(!network_publish("tap", "x", 1, 1, false));
            inject_recovery("snapshot", false); break;
        case 1:
            assert(network_receive(&m, 0) && m.length == 13);
            assert(!network_game_ready());
            assert(network_state_applied(&m)); break;
        case 2:
            assert(ack_sends == 1 && !network_game_ready());
            inject_recovery("resume", false); break;
        case 3:
            assert(network_game_ready());
            assert(network_publish("tap", "x", 1, 1, false));
            memcpy(previous_token, token, 8);
            ++generation; // Reconnect missed between owner iterations.
            inject_recovery("resume", true); break;
        case 4:
            assert(!network_game_ready() && sends == 0);
            assert(memcmp(token, previous_token, 8));
            inject_recovery("snapshot", false); break;
        case 5:
            assert(network_receive(&m, 0));
            assert(network_state_applied(&m)); send_ok = false; break;
        case 6:
            assert(!network_game_ready() && ack_sends == 2);
            send_ok = true; clock_ms += 200; break;
        case 7:
            assert(ack_sends == 3);
            inject_recovery("resume", false); break;
        case 8:
            assert(network_game_ready()); clock_ms += 3000; break;
        case 9:
            assert(!network_game_ready()); inject_recovery("snapshot", false); break;
        case 10:
            assert(network_receive(&m, 0)); assert(network_state_applied(&m)); break;
        case 11:
            inject_recovery("resume", false); break;
        case 12:
            assert(network_game_ready()); ++lower_drops; break;
        case 13:
            assert(!network_game_ready()); longjmp(iteration, 1);
        default: assert(false);
        }
        return;
    }
    switch (n) {
    case 0: {
        assert(network_game_ready());
        char bytes[] = "TAP";
        assert(network_publish("tap", bytes, 3, 1, false)); bytes[0] = 'X'; break;
    }
    case 1:
        assert(sends == 1 && !strcmp(received_payload, "TAP"));
        assert(!network_publish("bad/+", "x", 1, 1, false));
        assert(!network_publish("tap", NULL, 1, 1, false));
        assert(!network_publish("tap", "x", 1, 2, false));
        for (int i = 0; i < 8; ++i) assert(network_publish("tap", "x", 1, 1, false));
        assert(!network_publish("tap", "x", 1, 1, false)); ready = false; break;
    case 2:
        assert(network_drop_count() == 8 && !network_ready());
        ready = true; incoming = true; break;
    case 3:
        assert(network_receive(&m, 0) && m.length == 3 && m.payload[1] == 0);
        assert(!network_receive(&m, 20) && observed_wait == 20);
        assert(!network_receive(NULL, 0));
        assert(network_publish("tap", "x", 1, 1, false)); send_ok = false; break;
    case 4:
        assert(network_drop_count() == 9); incoming = true; break;
    case 13:
        assert(network_drop_count() == 10); ready = false; break;
    case 14:
        assert(network_drop_count() == 18 && !network_receive(&m, 0));
        longjmp(iteration, 1);
    default: assert(n >= 5 && n <= 12); incoming = true;
    }
}
int main(int argc, char **argv) {
    (void)argv;
    test_recovery = argc > 1;
    const char *filters[] = {"command", "nodes/+/status"};
    network_task_config_t config = {.ssid="ssid", .password="password", .client_id="id",
                                    .status_topic="status", .subscriptions=filters, .subscription_count=2};
    if (test_recovery) {
        config.sync_topic="sync"; config.state_topic="snapshot";
        config.applied_topic="applied"; config.resume_topic="resume"; config.boot_nonce=42;
    }
    assert(!network_publish("tap", "x", 1, 1, false));
    assert(!network_task_start(NULL));
    network_task_config_t invalid = config;
    invalid.subscription_count = MQTT_MAX_SUBSCRIPTIONS + 1;
    assert(!network_task_start(&invalid));
    invalid = config; invalid.status_topic = "bad/+";
    assert(!network_task_start(&invalid));
    assert(network_task_start(&config));
    assert(!network_task_start(&config));
    ready = true;
    // Run ONE continuous worker loop. Delay callbacks drive simulated application
    // actions; longjmp exits only after the entire scenario, not after each loop.
    if (!setjmp(iteration)) task_entry(task_arg);
    puts(test_recovery ? "Network task recovery integration tests passed." : "Network task queue tests passed.");
}
