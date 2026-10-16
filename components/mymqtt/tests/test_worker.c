#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#define time mock_time
#include "../mymqtt.c"
#undef time

const char mqtt_ca[] = "test CA";
const char mqtt_cert[] = "test client certificate";
const char mqtt_key[] = "test key";
static int64_t clock_ms;
static unsigned tasks, inits, starts, stops, uploads, heartbeats, ntp_inits;
static int64_t start_times[8], upload_times[64];
static unsigned temp_uploads;
static int message_id;
static event_handler_t handler;
static void (*task_fn)(void *);
static connection_event_t queue[32];
static unsigned queue_count;
static jmp_buf done;
static bool running;
static int scenario;
enum { NORMAL, NO_ACK, WIFI_DOWN, CONNECT_TIMEOUT, NO_CLOCK, OVERFLOW, OUTBOX_FULL,
       BROKER_DOWN, WIFI_FLAP, FRESH_DATA };

static void send_event(int id, int msg_id)
{
    esp_mqtt_event_t event = {.msg_id = msg_id};
    handler(NULL, NULL, id, &event);
}
time_t mock_time(time_t *out)
{
    time_t t = scenario == NO_CLOCK && clock_ms < 3000 ? 0 : 1791800000;
    if (out) *out = t;
    return t;
}
int64_t esp_timer_get_time(void) {return clock_ms * 1000;}
esp_err_t esp_read_mac(uint8_t *mac, int type)
{
    (void)type; memset(mac, 0xab, 6); return ESP_OK;
}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *cfg)
{
    assert(cfg->server != NULL); ++ntp_inits; return ESP_OK;
}
QueueHandle_t xQueueCreate(unsigned count, size_t size)
{
    assert(count == 32 && size == sizeof(connection_event_t)); return queue;
}
void vQueueDelete(QueueHandle_t q) {(void)q;}
void xQueueReset(QueueHandle_t q) {assert(q == queue); queue_count = 0;}
int xQueueSend(QueueHandle_t q, const void *event, unsigned wait)
{
    assert(q == queue && wait == 0);
    if (queue_count == 32) return 0;
    queue[queue_count++] = *(const connection_event_t *)event; return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *event, unsigned wait)
{
    assert(q == queue && wait == 0);
    if (!queue_count) return 0;
    *(connection_event_t *)event = queue[0];
    --queue_count; memmove(queue, queue + 1, queue_count * sizeof(*queue));
    return pdTRUE;
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                 unsigned priority, void *handle)
{
    (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    ++tasks; task_fn = fn; return pdPASS;
}
void vTaskDelete(void *task) {(void)task; assert(!"Unexpected fatal exit");}
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *cfg)
{
    assert(strncmp(cfg->broker.address.uri, "mqtts://", 8) == 0);
    assert(cfg->broker.address.port == 8883 && !cfg->broker.verification.skip_cert_common_name_check);
    assert(cfg->broker.verification.certificate == mqtt_ca);
    assert(cfg->credentials.authentication.certificate == mqtt_cert);
    assert(cfg->credentials.authentication.key == mqtt_key);
    ++inits; return &running;
}
esp_err_t esp_mqtt_client_register_event(void *c, int id, event_handler_t fn, void *arg)
{
    (void)c; (void)id; (void)arg; handler = fn; return ESP_OK;
}
esp_err_t esp_mqtt_client_destroy(void *c) {(void)c; return ESP_OK;}
esp_err_t esp_mqtt_client_start(void *c)
{
    assert(c != NULL && !running && network_ready);
    running = true; start_times[starts++] = clock_ms;
    assert(starts < 8);
    if (scenario != CONNECT_TIMEOUT) send_event(MQTT_EVENT_CONNECTED, 0);
    return ESP_OK;
}
esp_err_t esp_mqtt_client_stop(void *c)
{
    assert(c != NULL && running); running = false; ++stops;
    send_event(MQTT_EVENT_DISCONNECTED, 0); /* stop callback must be flushed */
    return ESP_OK;
}
int w25q64_cache_init(void) { return ESP_FAIL; }
int w25q64_cache_append(const void *data, size_t len, uint64_t *id)
{ (void)data; (void)len; (void)id; return FF_FULL; }
int w25q64_cache_peek(void *data, size_t cap, size_t *len, uint64_t *id)
{ (void)data; (void)cap; (void)len; (void)id; return FF_EMPTY; }
int w25q64_cache_ack(uint64_t id) { (void)id; return FF_EMPTY; }

esp_err_t modbusTCP_get_temp_sample(modbus_temp_sample_t *sample)
{
    *sample = (modbus_temp_sample_t){.value=250,.type=2,.has_value=true,.stale=true,
                                   .last_success_ms=0,.state=MODBUS_BACKOFF,.last_error=ESP_ERR_TIMEOUT};
    if (scenario == FRESH_DATA) {
        sample->value = 250 + clock_ms / 1000;
        sample->stale = false;
        sample->last_success_ms = clock_ms;
        sample->state = MODBUS_POLLING;
        sample->last_error = ESP_OK;
    }
    return ESP_OK;
}
int esp_mqtt_client_enqueue(void *c, const char *topic, const char *payload,
                            int len, int qos, int retain, bool store)
{
    assert(c != NULL && running && network_ready && qos == 1 && retain == 0 && store);
    assert(len == (int)strlen(payload));
    int id = ++message_id;
    if (!strcmp(topic, CONFIG_INGAWAY_MQTT_TOPIC_DATA)) {
        if (scenario == FRESH_DATA) {
            char expected[48];
            snprintf(expected, sizeof(expected), "\"temperature_raw\":%d", 250 + (int)(clock_ms / 1000));
            assert(strstr(payload, expected) && strstr(payload, "\"stale\":false"));
            assert(strstr(payload, "\"sample_age_ms\":0"));
        } else {
            assert(strstr(payload, "\"temperature_raw\":250") && strstr(payload, "\"stale\":true"));
        }
        upload_times[uploads++] = clock_ms;
        if (scenario == OUTBOX_FULL && uploads == 2) return -2;
        send_event(MQTT_EVENT_PUBLISHED, id); /* Must not acknowledge heartbeat. */
    } else if (!strcmp(topic, CONFIG_INGAWAY_MQTT_TOPIC_TEMP)) {
        char expected[16] = "null";
        if (scenario == FRESH_DATA) {
            snprintf(expected, sizeof(expected), "%d", 250 + (int)(clock_ms / 1000));
        }
        assert(!strcmp(payload, expected));
        assert(uploads > 0 && upload_times[uploads - 1] == clock_ms);
        ++temp_uploads;
        send_event(MQTT_EVENT_PUBLISHED, id);
    } else {
        assert(!strcmp(topic, CONFIG_INGAWAY_MQTT_TOPIC_HEARTBEAT));
        ++heartbeats;
        if (scenario != NO_ACK) send_event(MQTT_EVENT_PUBLISHED, id);
    }
    return id;
}
void vTaskDelay(uint32_t ticks)
{
    clock_ms += ticks;
    assert(clock_ms < 100000);
    if (scenario == WIFI_DOWN && clock_ms == 2000) mqtt_network_down();
    if (scenario == WIFI_DOWN && clock_ms == 5000) {mqtt_start(); mqtt_start();}
    if (scenario == BROKER_DOWN && clock_ms == 2000) send_event(MQTT_EVENT_DISCONNECTED, 0);
    if (scenario == WIFI_FLAP && clock_ms == 2000) {mqtt_network_down(); mqtt_start();}
    if (scenario == OVERFLOW && clock_ms == 1000) {
        for (int i=0;i<33;++i) send_event(MQTT_EVENT_PUBLISHED, 999);
    }
    if (((scenario == NORMAL || scenario == FRESH_DATA) && clock_ms >= 16000) ||
        (scenario == NO_CLOCK && uploads >= 2) ||
        ((scenario == BROKER_DOWN || scenario == WIFI_FLAP) && uploads >= 3) ||
        (scenario != NORMAL && scenario != FRESH_DATA && scenario != NO_CLOCK &&
         scenario != BROKER_DOWN && scenario != WIFI_FLAP && starts == 2)) longjmp(done, 1);
}
static void reset(int mode)
{
    scenario = mode;
    clock_ms=tasks=inits=starts=stops=uploads=heartbeats=ntp_inits=queue_count=0;
    message_id=0; temp_uploads=0; running=false;
    atomic_store(&worker_requested, false);
    event_overflow=false; network_ready=false; network_generation=0;
}
static void run(void)
{
    mqtt_start(); mqtt_start(); assert(tasks == 1);
    if (!setjmp(done)) task_fn(NULL);
    assert(inits == 1 && tasks == 1);
    assert(temp_uploads == uploads - (scenario == OUTBOX_FULL ? 1 : 0));
}
int main(void)
{
    reset(NORMAL); run();
    assert(uploads==4 && heartbeats==2 && stops==0);
    for (unsigned i=1;i<uploads;++i) assert(upload_times[i]-upload_times[i-1]==5000);
    reset(FRESH_DATA); run();
    assert(uploads==4 && stops==0);
    for (unsigned i=1;i<uploads;++i) assert(upload_times[i]-upload_times[i-1]==5000);
    reset(BROKER_DOWN); run();
    assert(stops==1 && starts==2 && start_times[1]>=12000 && uploads==3);
    assert(upload_times[2]-upload_times[1]==5000);
    reset(WIFI_FLAP); run();
    assert(stops==1 && starts==2 && start_times[1]>=12000 && uploads==3);
    assert(upload_times[2]-upload_times[1]==5000);
    reset(NO_ACK); run();
    assert(stops==1 && heartbeats==3 && start_times[1]>=40000);
    reset(WIFI_DOWN); run();
    assert(stops==1 && start_times[1]>=12000 && uploads==1);
    reset(CONNECT_TIMEOUT); run();
    assert(stops==1 && start_times[1]>=25000 && uploads==0);
    reset(NO_CLOCK); run();
    assert(ntp_inits==1 && start_times[0]>=3000);
    reset(OVERFLOW); run();
    assert(stops==1 && start_times[1]>=11000);
    reset(OUTBOX_FULL); run();
    assert(stops==1 && uploads==2 && start_times[1]>=15000);
    char payload[384];
    modbus_temp_sample_t empty = {0};
    assert(telemetry_format(payload,sizeof(payload),"esp32-test",100,&empty)>0);
    assert(strstr(payload,"\"temperature_raw\":null") && strstr(payload,"\"stale\":true"));
    assert(strstr(payload,"\"sample_age_ms\":null"));
    assert(telemetry_format(payload,2,"esp32-test",100,&empty)==-1);
    puts("PASS: 5s upload, TLS config, heartbeat ACKs, Wi-Fi/MQTT recovery, SNTP gate, overflow, stale payload");
    return 0;
}
