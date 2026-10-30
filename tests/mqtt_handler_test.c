#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mock_lwip.h"
#include "mqtt_handler.h"

static mqtt_client_t fake_client;
static int locks, subscribe_calls, connect_calls, allocations;
static uint32_t clock_ms;
static connection_cb_t connection_callback;
static publish_cb_t incoming_callback;
static data_cb_t data_callback;
static request_cb_t subscription_callback;
static void *subscription_arg;
static char subscribed_topic[96];
static err_t subscribe_result = ERR_OK;
static err_t publish_result = ERR_OK;
static u8_t published_qos, published_retain, subscribed_qos;
static request_cb_t publish_callback;
static void *publish_arg;
static int completion_result = 99;
static void completed(void *arg, int8_t result) {
    assert(arg == &completion_result);
    completion_result = result;
}
static int disconnect_calls;
static err_t dns_result = ERR_OK;
static err_t connect_result = ERR_OK;
static char last_client_id[64];
typedef struct {
    void (*callback)(const char *, const ip_addr_t *, void *);
    void *arg;
} dns_request_t;
static dns_request_t dns_requests[32];
static size_t dns_calls;

void cyw43_arch_lwip_begin(void) { assert(locks == 0); ++locks; }
void cyw43_arch_lwip_end(void) { assert(locks == 1); --locks; }
u32_t sys_now(void) { return clock_ms; }
mqtt_client_t *mqtt_client_new(void) { ++allocations; return &fake_client; }
int mqtt_client_is_connected(mqtt_client_t *c) { assert(locks); return c->connected; }
const char *ipaddr_ntoa(const ip_addr_t *p) { (void)p; return "test"; }
err_t dns_gethostbyname(const char *n, ip_addr_t *p, void (*cb)(const char *, const ip_addr_t *, void *), void *a) {
    (void)n;
    assert(locks == 1);
    assert(dns_calls < sizeof(dns_requests) / sizeof(dns_requests[0]));
    dns_requests[dns_calls++] = (dns_request_t){cb, a};
    p->unused = 0;
    return dns_result;
}
err_t mqtt_client_connect(mqtt_client_t *c, const ip_addr_t *p, u16_t port, connection_cb_t cb, void *a, const struct mqtt_connect_client_info_t *info) {
    (void)c; (void)p; (void)port; (void)a; assert(info->client_id); assert(locks);
    assert(!strcmp(info->will_topic, "test/status"));
    assert(!strcmp(info->will_msg, "offline"));
    assert(info->will_qos == 1 && info->will_retain == 1 && info->keep_alive == 10);
    assert(strlen(info->client_id) < sizeof(last_client_id));
    strcpy(last_client_id, info->client_id);
    ++connect_calls;
    connection_callback = cb;
    return connect_result;
}
void mqtt_set_inpub_callback(mqtt_client_t *c, publish_cb_t p, data_cb_t d, void *a) {
    (void)c; (void)a; incoming_callback = p; data_callback = d;
}
err_t mqtt_subscribe(mqtt_client_t *c, const char *t, u8_t qos, request_cb_t cb, void *a) {
    (void)c; assert(locks); subscribed_qos = qos; ++subscribe_calls;
    strcpy(subscribed_topic, t); subscription_callback = cb; subscription_arg = a;
    return subscribe_result;
}
err_t mqtt_publish(mqtt_client_t *c, const char *t, const void *p, u16_t len, u8_t qos, u8_t retain, request_cb_t cb, void *a) {
    (void)c; (void)t; (void)p; (void)len; (void)cb; (void)a;
    assert(locks); published_qos = qos; published_retain = retain;
    publish_callback = cb; publish_arg = a; return publish_result;
}
static void acknowledge(err_t result) {
    cyw43_arch_lwip_begin(); subscription_callback(subscription_arg, result); cyw43_arch_lwip_end();
}
static void connection_event(int status) {
    cyw43_arch_lwip_begin();
    fake_client.connected = status == MQTT_CONNECT_ACCEPTED;
    connection_callback(&fake_client, NULL, status);
    cyw43_arch_lwip_end();
}
static void inject(const char *topic, const uint8_t *bytes, size_t length) {
    cyw43_arch_lwip_begin();
    incoming_callback(NULL, topic, (u32_t)length);
    data_callback(NULL, bytes, (u16_t)length, MQTT_DATA_FLAG_LAST);
    cyw43_arch_lwip_end();
}
void mqtt_disconnect(mqtt_client_t *client) {
    assert(locks == 1);
    assert(client == &fake_client);

    ++disconnect_calls;
    client->connected = 0;
}
// Deliver delayed DNS replies under the same lock used by the real SDK.
static void dns_reply(size_t request, bool found) {
    assert(request < dns_calls);
    ip_addr_t address = {0};
    cyw43_arch_lwip_begin();
    dns_requests[request].callback("test-broker", found ? &address : NULL,
                                   dns_requests[request].arg);
    cyw43_arch_lwip_end();
}

// These cases run before the subscription-limit test, with two saved filters.
static void finish_reconnect(void) {
    int calls_before = subscribe_calls;
    connection_event(MQTT_CONNECT_ACCEPTED);
    assert(subscribe_calls == calls_before + 1);
    assert(strcmp(subscribed_topic, "arbitrary/input") == 0 && subscribed_qos == 0);
    assert(!mqtt_is_ready());
    acknowledge(ERR_OK);
    assert(!mqtt_is_ready());
    init_mqtt_client("test-client");
    assert(subscribe_calls == calls_before + 2);
    assert(strcmp(subscribed_topic, "events/+/state") == 0 && subscribed_qos == 1);
    acknowledge(ERR_OK);
    assert(mqtt_is_ready());
}

static void test_explicit_reset(void) {
    mqtt_message_t message;
    const uint8_t bytes[] = {1, 0, 2};
    int disconnects_before = disconnect_calls;
    int connects_before = connect_calls;
    uint32_t drops_before = mqtt_received_drop_count();
    inject("stale", bytes, sizeof(bytes));
    cyw43_arch_lwip_begin();
    incoming_callback(NULL, "partial", 4);
    data_callback(NULL, bytes, 1, 0);
    cyw43_arch_lwip_end();

    mqtt_reset_connection();
    assert(disconnect_calls == disconnects_before + 1);
    assert(!is_mqtt_connected() && !mqtt_is_ready());
    assert(!mqtt_receive(&message));
    assert(mqtt_received_drop_count() == drops_before + 2);
    assert(!mqtt_send("offline", bytes, sizeof(bytes)));

    // Repeated reset must not count the same discarded messages twice.
    mqtt_reset_connection();
    assert(mqtt_received_drop_count() == drops_before + 2);
    init_mqtt_client("test-client");
    assert(connect_calls == connects_before);
    clock_ms += 4999;
    init_mqtt_client("test-client");
    assert(connect_calls == connects_before);
    ++clock_ms;
    init_mqtt_client("test-client");
    assert(connect_calls == connects_before + 1 && allocations == 1);
    finish_reconnect();

    // Fresh traffic still works after all old receive state was removed.
    inject("fresh", bytes, sizeof(bytes));
    assert(mqtt_receive(&message));
    assert(message.length == sizeof(bytes) && !strcmp(message.topic, "fresh"));
    assert(!memcmp(message.payload, bytes, sizeof(bytes)));
    puts("PASS: reset, stale-message cleanup, retry boundary, resubscription and fresh traffic");
}

static void test_cancelled_dns(void) {
    mqtt_reset_connection();
    clock_ms += 5000;
    dns_result = ERR_INPROGRESS;
    int connects_before = connect_calls;
    size_t old_lookup = dns_calls;
    init_mqtt_client("old-client");
    assert(dns_calls == old_lookup + 1 && connect_calls == connects_before);
    // Calling service again must not start overlapping lookups.
    init_mqtt_client("old-client");
    assert(dns_calls == old_lookup + 1);

    mqtt_reset_connection();
    dns_reply(old_lookup, true);
    assert(connect_calls == connects_before);
    clock_ms += 5000;
    size_t fresh_lookup = dns_calls;
    init_mqtt_client("fresh-client");
    assert(dns_calls == fresh_lookup + 1);
    // Neither a successful nor failed OLD reply may affect the fresh lookup.
    dns_reply(old_lookup, true);
    dns_reply(old_lookup, false);
    init_mqtt_client("fresh-client");
    assert(connect_calls == connects_before && dns_calls == fresh_lookup + 1);
    dns_reply(fresh_lookup, true);
    assert(connect_calls == connects_before + 1);
    assert(strcmp(last_client_id, "fresh-client") == 0);
    dns_result = ERR_OK;
    finish_reconnect();
    puts("PASS: cancelled/stale DNS cannot revive or cancel a newer attempt");
}

static void test_reset_during_connect_and_subscribe(void) {
    mqtt_reset_connection();
    clock_ms += 5000;
    init_mqtt_client("test-client"); // Connect started, no acceptance yet.
    int connects_before = connect_calls;
    mqtt_reset_connection();
    assert(!is_mqtt_connected() && !mqtt_is_ready());
    clock_ms += 5000;
    init_mqtt_client("test-client");
    assert(connect_calls == connects_before + 1);
    connection_event(MQTT_CONNECT_ACCEPTED); // First SUBACK is still pending.
    assert(!mqtt_is_ready());
    mqtt_reset_connection();
    // Real lwIP cancels pending requests: do not invent a late SUBACK here.
    clock_ms += 5000;
    init_mqtt_client("test-client");
    finish_reconnect();
    puts("PASS: reset during TCP/MQTT connect and pending subscription");
}

static void test_dns_and_connect_failures(void) {
    mqtt_reset_connection();
    clock_ms += 5000;
    dns_result = ERR_MEM;
    size_t calls_before = dns_calls;
    int connects_before = connect_calls;
    init_mqtt_client("test-client");
    assert(dns_calls == calls_before + 1 && connect_calls == connects_before);
    clock_ms += 4999;
    init_mqtt_client("test-client");
    assert(dns_calls == calls_before + 1);
    ++clock_ms;
    dns_result = ERR_INPROGRESS;
    init_mqtt_client("test-client");
    dns_reply(dns_calls - 1, false); // Resolver completes without an address.
    clock_ms += 5000;
    dns_result = ERR_OK;
    connect_result = ERR_MEM;
    init_mqtt_client("test-client");
    assert(connect_calls == connects_before + 1);
    init_mqtt_client("test-client");
    assert(connect_calls == connects_before + 1);
    clock_ms += 5000;
    connect_result = ERR_OK;
    init_mqtt_client("test-client");
    assert(connect_calls == connects_before + 2);
    finish_reconnect();
    puts("PASS: DNS start/final failure and immediate connect failure remain retryable");
}

static void test_retry_clock_wrap(void) {
    clock_ms = UINT32_MAX - 1000;
    mqtt_reset_connection();
    int connects_before = connect_calls;
    clock_ms += 4999; // Unsigned clock wraps here.
    init_mqtt_client("test-client");
    assert(connect_calls == connects_before);
    ++clock_ms;
    init_mqtt_client("test-client");
    assert(connect_calls == connects_before + 1);
    finish_reconnect();
    assert(allocations == 1);
    puts("PASS: retry timing across clock wrap; client and Will configuration reused");
}

int main(void) {
    mqtt_message_t message;
    mqtt_reset_connection(); // Safe before client allocation.
    assert(disconnect_calls == 0 && !is_mqtt_connected());
    assert(!mqtt_receive(NULL) && !mqtt_receive(&message));
    assert(!mqtt_send("test", "x", 1));
    assert(!mqtt_subscribe_topic(NULL));
    assert(!mqtt_subscribe_topic(""));
    assert(!mqtt_subscribe_topic("bad#"));
    assert(!mqtt_subscribe_topic("bad/+suffix"));
    char supplied[] = "arbitrary/input";
    assert(mqtt_subscribe_topic(supplied));
    supplied[0] = 'X'; // Framework owns a copy, not the caller's buffer.
    assert(mqtt_subscribe_topic("arbitrary/input"));
    assert(!mqtt_configure_will("bad/+", "offline", 10));
    assert(!mqtt_configure_will("test/status", NULL, 10));
    assert(!mqtt_configure_will("test/status", "offline", 0));
    char will_topic[] = "test/status", will_payload[] = "offline";
    assert(mqtt_configure_will(will_topic, will_payload, 10));
    will_topic[0] = 'X'; will_payload[0] = 'X';
    init_mqtt_client("test-client");
    init_mqtt_client("test-client");
    assert(connect_calls == 1 && allocations == 1);
    assert(!mqtt_configure_will("other", "offline", 10));
    connection_event(MQTT_CONNECT_ACCEPTED);
    assert(!mqtt_is_ready());
    assert(strcmp(subscribed_topic, "arbitrary/input") == 0);
    acknowledge(ERR_MEM);
    init_mqtt_client("test-client");
    assert(subscribe_calls == 1);
    clock_ms = 1000;
    init_mqtt_client("test-client");
    assert(subscribe_calls == 2);
    acknowledge(ERR_OK);
    assert(mqtt_is_ready());

    assert(mqtt_subscribe_topic_qos("events/+/state", 1)); // Add while connected.
    assert(!mqtt_subscribe_topic_qos("events/+/state", 0));
    assert(!mqtt_subscribe_topic_qos("invalid/qos", 2));
    assert(!mqtt_is_ready());
    subscribe_result = ERR_MEM;
    init_mqtt_client("test-client");
    subscribe_result = ERR_OK;
    clock_ms += 1000;
    init_mqtt_client("test-client");
    assert(subscribed_qos == 1);
    acknowledge(ERR_OK);
    assert(mqtt_is_ready());

    // Fragmented binary payload is not delivered until complete.
    const uint8_t bytes[] = {'A', 0, 'B', 255};
    cyw43_arch_lwip_begin();
    incoming_callback(NULL, "events/2/state", 4);
    data_callback(NULL, bytes, 2, 0);
    cyw43_arch_lwip_end();
    assert(!mqtt_receive(&message));
    cyw43_arch_lwip_begin(); data_callback(NULL, bytes + 2, 2, MQTT_DATA_FLAG_LAST); cyw43_arch_lwip_end();
    assert(mqtt_receive(&message));
    assert(!strcmp(message.topic, "events/2/state") && message.length == 4);
    assert(memcmp(message.payload, bytes, 4) == 0);
    inject("empty", NULL, 0);
    assert(mqtt_receive(&message) && message.length == 0);
    assert(!mqtt_receive(&message));

    // FIFO order, full queue drops newest, and queue wraps after consumption.
    for (uint8_t n = 0; n < MQTT_RX_QUEUE_CAPACITY + 1; ++n) inject("fifo", &n, 1);
    assert(mqtt_received_drop_count() == 1);
    for (uint8_t n = 0; n < MQTT_RX_QUEUE_CAPACITY; ++n) {
        assert(mqtt_receive(&message) && message.payload[0] == n);
    }
    inject("again", bytes, 4);
    assert(mqtt_receive(&message) && message.length == 4);

    char max_topic[MQTT_TOPIC_MAX_LENGTH + 1];
    memset(max_topic, 't', sizeof(max_topic) - 1); max_topic[sizeof(max_topic) - 1] = 0;
    uint8_t max_payload[MQTT_PAYLOAD_MAX_LENGTH + 1] = {0};
    inject(max_topic, max_payload, MQTT_PAYLOAD_MAX_LENGTH);
    assert(mqtt_receive(&message) && message.length == MQTT_PAYLOAD_MAX_LENGTH);
    inject("too-big", max_payload, sizeof(max_payload));
    assert(!mqtt_receive(&message));
    cyw43_arch_lwip_begin();
    incoming_callback(NULL, "short", 4);
    data_callback(NULL, bytes, 2, MQTT_DATA_FLAG_LAST);
    incoming_callback(NULL, "long", 2);
    data_callback(NULL, bytes, 4, MQTT_DATA_FLAG_LAST);
    cyw43_arch_lwip_end();
    assert(!mqtt_receive(&message));
    assert(mqtt_received_drop_count() == 4);
    assert(mqtt_send(max_topic, max_payload, MQTT_PAYLOAD_MAX_LENGTH));
    assert(mqtt_send("empty", NULL, 0));
    assert(published_qos == 0 && published_retain == 0);
    assert(mqtt_send_ex("state", bytes, 4, 1, true, completed, &completion_result));
    assert(published_qos == 1 && published_retain == 1 && completion_result == 99);
    cyw43_arch_lwip_begin(); publish_callback(publish_arg, ERR_OK); cyw43_arch_lwip_end();
    assert(completion_result == ERR_OK);
    assert(mqtt_send_ex("state", NULL, 0, 1, true, NULL, NULL));
    assert(!mqtt_send_ex("state", bytes, 4, 2, false, NULL, NULL));
    assert(!mqtt_send("bad/+", bytes, 4));
    assert(!mqtt_send("bad", NULL, 1));
    assert(!mqtt_send("bad", max_payload, sizeof(max_payload)));
    publish_result = ERR_MEM; assert(!mqtt_send("busy", bytes, 4)); publish_result = ERR_OK;

    // Disconnect discards both complete and partial old-session messages.
    inject("stale", bytes, 4);
    cyw43_arch_lwip_begin(); incoming_callback(NULL, "partial", 4); cyw43_arch_lwip_end();
    connection_event(MQTT_CONNECT_DISCONNECTED);
    assert(!mqtt_receive(&message) && !mqtt_is_ready());
    assert(mqtt_received_drop_count() == 6);
    init_mqtt_client("test-client"); assert(connect_calls == 1);
    clock_ms += 5000;
    init_mqtt_client("test-client"); assert(connect_calls == 2 && allocations == 1);
    connection_event(MQTT_CONNECT_ACCEPTED);
    acknowledge(ERR_OK);
    assert(!mqtt_is_ready());
    init_mqtt_client("test-client"); acknowledge(ERR_OK);
    assert(mqtt_is_ready());

    test_explicit_reset();
    test_cancelled_dns();
    test_reset_during_connect_and_subscribe();
    test_dns_and_connect_failures();
    test_retry_clock_wrap();

    // Exactly eight unique subscriptions, including wildcards.
    for (unsigned n = 2; n < MQTT_MAX_SUBSCRIPTIONS; ++n) {
        char topic[32]; snprintf(topic, sizeof(topic), "module/%u/#", n);
        assert(mqtt_subscribe_topic(topic));
    }
    assert(!mqtt_subscribe_topic("one-too-many"));
    assert(mqtt_subscribe_topic("arbitrary/input"));
    assert(locks == 0);
    puts("MQTT framework tests passed.");
}
