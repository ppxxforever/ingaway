#include "mymqtt.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <inttypes.h>
#include "modbusTCP.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "heartbeat.h"
#include "telemetry.h"
#include "w25q64_cache.h"
#include "storage_archive.h"
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define UPLOAD_INTERVAL_MS 5000
#define HEARTBEAT_INTERVAL_MS 10000
#define RECONNECT_INTERVAL_MS 10000
#define CONNECT_DEADLINE_MS 15000
#define WORKER_TICK_MS 200
#define RAM_CACHE_CAP 16
#define CACHE_TOPIC_MAX 128
#define CACHE_PAYLOAD_MAX 640
#define RECORD_ACK_TIMEOUT_MS 30000
#define CACHE_RECOVERY_MS 5000
#define CACHE_RECORD_MAX (4 + CACHE_TOPIC_MAX + CACHE_PAYLOAD_MAX)

extern const char mqtt_ca[] asm("_binary_ca_crt_start");
extern const char mqtt_cert[] asm("_binary_client_crt_start");
extern const char mqtt_key[] asm("_binary_client_key_start");

static const char *TAG = "mqtt";
static atomic_bool worker_requested;
static QueueHandle_t events;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool event_overflow;
static bool network_ready;
static uint32_t network_generation;
typedef struct { int id; int msg_id; } connection_event_t;
typedef struct {
    uint64_t id;
    char topic[CACHE_TOPIC_MAX];
    char payload[CACHE_PAYLOAD_MAX];
    int len;
    bool used;
} ram_record_t;
/* One data record in flight, matching the Flash FIFO's oldest-only ACK API.
 * The MQTT outbox owns retries within a session; this mapping owns release. */
typedef struct {
    bool used;
    bool flash;
    bool ack_received;
    uint64_t record_id;
    int msg_id;
    int64_t deadline;
    char wire_id[64];
} inflight_record_t;
static ram_record_t ram_cache[RAM_CACHE_CAP];
static inflight_record_t active_record;
static uint64_t next_record_sequence = 1;
static uint32_t boot_nonce[4];
static bool flash_cache_ready;
static int64_t next_cache_recovery;

/* Reuse the existing CA and client identity. Never disable peer-name checks. */
static const esp_mqtt_client_config_t mqtt_cfg = {
    .broker = {
        .address.uri = CONFIG_INGAWAY_MQTT_BROKER_URI,
        .address.port = CONFIG_INGAWAY_MQTT_BROKER_PORT,
        .verification.certificate = mqtt_ca,
        .verification.skip_cert_common_name_check = false,
    },
    .credentials.authentication = { .certificate = mqtt_cert, .key = mqtt_key },
    .session.keepalive = 30,
    .network = { .disable_auto_reconnect = true, .timeout_ms = 5000 },
    .outbox.limit = 4096,
};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool get_network(uint32_t *generation)
{
    portENTER_CRITICAL(&state_lock);
    bool ready = network_ready;
    *generation = network_generation;
    portEXIT_CRITICAL(&state_lock);
    return ready;
}

static bool take_overflow(void)
{
    portENTER_CRITICAL(&state_lock);
    bool overflow = event_overflow;
    event_overflow = false;
    portEXIT_CRITICAL(&state_lock);
    return overflow;
}

static int pack_record(uint8_t *out, size_t cap, const char *topic, const char *payload, int len)
{
    size_t topic_len = strlen(topic);
    if (topic_len >= CACHE_TOPIC_MAX || len < 0 || len > CACHE_PAYLOAD_MAX ||
        4u + topic_len + (size_t)len > cap) {
        return -1;
    }
    out[0] = (uint8_t)(topic_len >> 8);
    out[1] = (uint8_t)topic_len;
    out[2] = (uint8_t)((unsigned)len >> 8);
    out[3] = (uint8_t)len;
    memcpy(out + 4, topic, topic_len);
    memcpy(out + 4 + topic_len, payload, len);
    return (int)(4u + topic_len + (size_t)len);
}

static bool unpack_record(const uint8_t *in, size_t size, char *topic, size_t topic_cap,
                          char *payload, size_t payload_cap, int *len)
{
    if (size < 4) return false;
    size_t topic_len = ((size_t)in[0] << 8) | in[1];
    size_t payload_len = ((size_t)in[2] << 8) | in[3];
    if (topic_len == 0 || topic_len >= topic_cap || payload_len == 0 ||
        payload_len > payload_cap || 4u + topic_len + payload_len != size) {
        return false;
    }
    if (memchr(in + 4, 0, topic_len + payload_len) != NULL) return false;
    memcpy(topic, in + 4, topic_len);
    topic[topic_len] = '\0';
    memcpy(payload, in + 4 + topic_len, payload_len);
    if (payload_len < payload_cap) payload[payload_len] = '\0';
    *len = (int)payload_len;
    return true;
}

static void flash_failed(int rc)
{
    flash_cache_ready = false;
    next_cache_recovery = now_ms() + CACHE_RECOVERY_MS;
    ESP_LOGW(TAG, "Flash cache unavailable rc=%d; retain records and rescan", rc);
    storage_log(STORAGE_WARN,"mqtt","flash_cache_failed",rc,0,"");
}

static void recover_flash_cache(void)
{
    if (flash_cache_ready || now_ms() < next_cache_recovery) return;
    next_cache_recovery = now_ms() + CACHE_RECOVERY_MS;
    if (w25q64_cache_init() == ESP_OK) {
        flash_cache_ready = true;
        ESP_LOGI(TAG, "External MQTT cache recovered");
        storage_log(STORAGE_INFO,"mqtt","flash_cache_recovered",0,0,"");
    }
}

static unsigned oldest_ram(void)
{
    unsigned best = RAM_CACHE_CAP;
    for (unsigned i = 0; i < RAM_CACHE_CAP; ++i) {
        if (ram_cache[i].used && (best == RAM_CACHE_CAP || ram_cache[i].id < ram_cache[best].id)) {
            best = i;
        }
    }
    return best;
}

static void reset_inflight_after_disconnect(void)
{
    /* A received ACK awaiting durable Flash release survives network restart.
     * Otherwise destroy has removed the old outbox, so replay from the cache. */
    if (!active_record.ack_received) active_record = (inflight_record_t){0};
}

/* Persist the wire ID in the payload itself. A 128-bit random boot nonce plus
 * a monotonically increasing sequence avoids ID reuse on reboot/RAM fallback.
 * MAC/device_id scopes it across devices. Never regenerate it for a retry. */
static int cache_record(const char *device_id, const char *topic, const char *payload, int len)
{
    if (!payload || len <= 0 || next_record_sequence == UINT64_MAX) return -1;
    uint64_t seq = next_record_sequence++;
    char wire_id[64], envelope[CACHE_PAYLOAD_MAX];
    snprintf(wire_id, sizeof(wire_id), "%08" PRIx32 "%08" PRIx32 "%08" PRIx32
             "%08" PRIx32 "-%016" PRIx64,
             boot_nonce[0], boot_nonce[1], boot_nonce[2], boot_nonce[3], seq);
    int envelope_len;
    if (payload[0] == '{') {
        /* Keep existing telemetry fields at top level. Input is our own JSON. */
        envelope_len = snprintf(envelope, sizeof(envelope),
                                "{\"record_id\":\"%s\",%.*s", wire_id, len - 1, payload + 1);
    } else {
        envelope_len = snprintf(envelope, sizeof(envelope),
                                "{\"record_id\":\"%s\",\"device_id\":\"%s\",\"temperature_raw\":%.*s}",
                                wire_id, device_id, len, payload);
    }
    if (envelope_len < 0 || (size_t)envelope_len >= sizeof(envelope)) return -1;
    uint8_t packed[CACHE_RECORD_MAX];
    int packed_len = pack_record(packed, sizeof(packed), topic, envelope, envelope_len);
    if (packed_len < 0) return -1;
    /* Do not let new Flash writes overtake an older RAM fallback backlog. */
    if (flash_cache_ready && oldest_ram() == RAM_CACHE_CAP) {
        uint64_t id;
        int rc = w25q64_cache_append(packed, (size_t)packed_len, &id);
        if (rc == FF_OK) return 0;
        if (rc != FF_FULL && rc != FF_EXHAUSTED) flash_failed(rc);
    }
    for (unsigned i = 0; i < RAM_CACHE_CAP; ++i) {
        if (!ram_cache[i].used) {
            ram_cache[i].id = seq;
            snprintf(ram_cache[i].topic, sizeof(ram_cache[i].topic), "%s", topic);
            memcpy(ram_cache[i].payload, envelope, (size_t)envelope_len);
            ram_cache[i].len = envelope_len;
            ram_cache[i].used = true;
            return 0;
        }
    }
    ESP_LOGW(TAG, "Flash/RAM cache full; rejecting new record %s", wire_id);
    storage_log(STORAGE_WARN,"mqtt","cache_full",FF_FULL,seq,wire_id);
    return -1;
}

static void log_delivery(storage_level_t level, const char *event, int error,
                         int msg_id, uint64_t id, const char *wire_id)
{
    char detail[96];
    snprintf(detail,sizeof(detail),"msg_id=%d wire_id=%s",msg_id,wire_id);
    storage_log(level,"mqtt",event,error,id,detail);
}

static bool ack_cached_record(int msg_id)
{
    if (!active_record.used || active_record.ack_received || active_record.msg_id != msg_id) {
        return false;
    }
    log_delivery(STORAGE_INFO,active_record.flash?"flash_replay_puback":"ram_replay_puback",
                 0,msg_id,active_record.record_id,active_record.wire_id);
    active_record.ack_received = true;
    active_record.msg_id = -1;
    if (!active_record.flash) {
        for (unsigned i = 0; i < RAM_CACHE_CAP; ++i) {
            if (ram_cache[i].used && ram_cache[i].id == active_record.record_id) {
                ram_cache[i] = (ram_record_t){0};
                break;
            }
        }
        active_record = (inflight_record_t){0};
    }
    return true;
}

/* Durable release can be retried without a second network publish. A write
 * error may have completed the ACK marker: rescan/peek distinguishes this. */
static void finish_flash_ack(void)
{
    if (!active_record.used || !active_record.ack_received || !flash_cache_ready) return;
    uint8_t packed[CACHE_RECORD_MAX];
    size_t len;
    uint64_t id;
    int rc = w25q64_cache_peek(packed, sizeof(packed), &len, &id);
    if (rc == FF_EMPTY || (rc == FF_OK && id > active_record.record_id)) {
        storage_log(STORAGE_DEBUG,"mqtt","flash_release_recovered",0,
                    active_record.record_id,active_record.wire_id);
        active_record = (inflight_record_t){0};
        return;
    }
    if (rc != FF_OK || id != active_record.record_id) {
        flash_failed(rc == FF_OK ? FF_CORRUPT : rc);
        return;
    }
    rc = w25q64_cache_ack(id);
    if (rc == FF_OK) {
        storage_log(STORAGE_DEBUG,"mqtt","flash_release_complete",0,id,active_record.wire_id);
        active_record = (inflight_record_t){0};
    } else {
        storage_log(STORAGE_WARN,"mqtt","flash_release_failed",rc,id,active_record.wire_id);
        flash_failed(rc);
    }
}

static void pump_cached_records(esp_mqtt_client_handle_t client, const char *device_id)
{
    if (active_record.used) return;
    char topic[CACHE_TOPIC_MAX], payload[CACHE_PAYLOAD_MAX];
    uint8_t packed[CACHE_RECORD_MAX];
    size_t packed_len;
    uint64_t id = 0;
    int len = 0;
    bool flash = false;
    if (flash_cache_ready) {
        int rc = w25q64_cache_peek(packed, sizeof(packed), &packed_len, &id);
        if (rc == FF_OK) {
            if (!unpack_record(packed, packed_len, topic, sizeof(topic), payload, sizeof(payload), &len)) {
                flash_failed(FF_CORRUPT);
                return;
            }
            flash = true;
        } else if (rc != FF_EMPTY) {
            flash_failed(rc);
            return;
        }
    }
    if (!flash) {
        unsigned r = oldest_ram();
        if (r == RAM_CACHE_CAP) return;
        snprintf(topic, sizeof(topic), "%s", ram_cache[r].topic);
        len = ram_cache[r].len;
        memcpy(payload, ram_cache[r].payload, (size_t)len);
        id = ram_cache[r].id;
        /* Move RAM fallback to durable storage when capacity becomes available.
         * Preserve the exact wire ID/payload. On uncertain writes keep RAM too;
         * the receiver deduplicates by wire record_id, not the physical slot ID. */
        if (flash_cache_ready) {
            int n = pack_record(packed, sizeof(packed), topic, payload, len);
            uint64_t flash_id;
            int rc = n < 0 ? FF_ARGUMENT : w25q64_cache_append(packed, (size_t)n, &flash_id);
            if (rc == FF_OK) {
                ram_cache[r] = (ram_record_t){0};
                id = flash_id;
                flash = true;
                storage_log(STORAGE_DEBUG,"mqtt","ram_to_flash",0,id,"");
            } else if (rc != FF_FULL && rc != FF_EXHAUSTED) {
                flash_failed(rc);
                return;
            }
        }
    }
    /* Old packed records predate wire IDs. Derive a stable legacy identity
     * from the persistent Flash ID without modifying the stored record. */
    static const char id_prefix[] = "{\"record_id\":\"";
    if (flash && (len < (int)(sizeof(id_prefix) - 1) ||
                  memcmp(payload, id_prefix, sizeof(id_prefix) - 1) != 0)) {
        char legacy[CACHE_PAYLOAD_MAX];
        int n;
        if (payload[0] == '{') {
            n = snprintf(legacy, sizeof(legacy), "{\"record_id\":\"flash-%016" PRIx64 "\",%.*s",
                         id, len - 1, payload + 1);
        } else {
            n = snprintf(legacy, sizeof(legacy),
                         "{\"record_id\":\"flash-%016" PRIx64 "\",\"device_id\":\"%s\","
                         "\"temperature_raw\":%.*s}", id, device_id, len, payload);
        }
        if (n < 0 || (size_t)n >= sizeof(legacy)) {
            flash_failed(FF_ARGUMENT);
            return;
        }
        memcpy(payload, legacy, (size_t)n);
        len = n;
    }
    /* Reserve the single slot before enqueue. Callbacks only queue events;
     * the worker cannot process an ACK until this mapping is installed. */
    int msg_id = esp_mqtt_client_enqueue(client, topic, payload, len, 1, 0, true);
    if (msg_id < 0) {
        storage_log(STORAGE_DEBUG,"mqtt","replay_enqueue_backpressure",msg_id,id,"");
        return; /* Backpressure: record remains queued. */
    }
    active_record = (inflight_record_t){ .used = true, .flash = flash,
        .record_id = id, .msg_id = msg_id, .deadline = now_ms() + RECORD_ACK_TIMEOUT_MS };
    size_t prefix_len=sizeof(id_prefix)-1;
    if ((size_t)len>prefix_len && memcmp(payload,id_prefix,prefix_len)==0) {
        const char *end=memchr(payload+prefix_len,'"',(size_t)len-prefix_len);
        if (end && (size_t)(end-payload-prefix_len)<sizeof(active_record.wire_id)) {
            memcpy(active_record.wire_id,payload+prefix_len,(size_t)(end-payload-prefix_len));
        }
    }
    log_delivery(STORAGE_DEBUG,"replay_enqueued",0,msg_id,id,active_record.wire_id);
}

/* No lifecycle operations or blocking waits inside MQTT callbacks. */
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t event = data;
    if (id == MQTT_EVENT_ERROR && event->error_handle) {
        ESP_LOGE(TAG, "MQTT error type=%d tls=0x%x stack=%d",
                 event->error_handle->error_type,
                 (unsigned)event->error_handle->esp_tls_last_esp_err,
                 event->error_handle->esp_tls_stack_err);
        storage_log(STORAGE_ERROR,"mqtt","tls_or_transport_error",
                    event->error_handle->esp_tls_last_esp_err,
                    event->error_handle->error_type,"see console for transport detail");
    }
    if (id != MQTT_EVENT_CONNECTED && id != MQTT_EVENT_DISCONNECTED &&
        id != MQTT_EVENT_PUBLISHED) {
        return;
    }
    connection_event_t message = { .id = id, .msg_id = event->msg_id };
    if (xQueueSend(events, &message, 0) != pdTRUE) {
        portENTER_CRITICAL(&state_lock);
        event_overflow = true;
        portEXIT_CRITICAL(&state_lock);
    }
}

static bool clock_ready(void)
{
    /* Clock plausibility gate only; TLS still validates actual cert dates. */
    return time(NULL) >= 1704067200; /* 2024-01-01 UTC */
}

static int cache_environment(const char *device_id)
{
    modbus_temp_sample_t sample;
    if (modbusTCP_get_temp_sample(&sample) != ESP_OK) {
        return -1;
    }
    char payload[384];
    int len = telemetry_format(payload, sizeof(payload), device_id, now_ms(), &sample);
    if (len < 0 || cache_record(device_id, CONFIG_INGAWAY_MQTT_TOPIC_DATA, payload, len) < 0) {
        return -1;
    }
    /* Same snapshot/cadence as telemetry. Never present stale data as current.
     * Register scaling is device-specific: publish the unscaled U16 value. */
    char temperature[16];
    int temp_len = sample.has_value && !sample.stale
        ? snprintf(temperature, sizeof(temperature), "%u", (unsigned)sample.value)
        : snprintf(temperature, sizeof(temperature), "null");
    if (temp_len < 0 || cache_record(device_id, CONFIG_INGAWAY_MQTT_TOPIC_TEMP, temperature, temp_len) < 0) {
        return -1;
    }
    return 0;
}

static void mqtt_worker(void *arg)
{
    (void)arg;
    esp_mqtt_client_handle_t client = NULL;
    bool started = false, connected = false, sntp_initialized = false;
    uint32_t active_generation = 0;
    int64_t next_connect = 0, connect_deadline = 0;
    int64_t next_upload = 0, next_heartbeat = 0, next_clock_notice = 0;
    heartbeat_t heartbeat;
    heartbeat_reset(&heartbeat);
    esp_fill_random(boot_nonce, sizeof(boot_nonce));
    recover_flash_cache();
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read device identity");
        vTaskDelete(NULL);
        return;
    }
    char device_id[32];
    snprintf(device_id, sizeof(device_id), "esp32-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    for (;;) {
        connection_event_t event;
        bool restart = take_overflow();
        if (restart) storage_log(STORAGE_WARN,"mqtt","event_queue_overflow",ESP_ERR_NO_MEM,0,"");
        uint32_t generation;
        bool ready = get_network(&generation);
        if (started && (!ready || generation != active_generation)) {
            restart = true;
        }
        /* Drain events before deadline checks so an already queued ACK wins. */
        while (xQueueReceive(events, &event, 0) == pdTRUE) {
            if (!started || restart) {
                continue;
            }
            if (event.id == MQTT_EVENT_CONNECTED) {
                connected = true;
                heartbeat_reset(&heartbeat);
                next_heartbeat = now_ms();
                ESP_LOGI(TAG, "MQTT connected; upload every 5 seconds");
                storage_log(STORAGE_INFO,"mqtt","connected",0,0,"");
            } else if (event.id == MQTT_EVENT_DISCONNECTED) {
                storage_log(STORAGE_WARN,"mqtt","disconnected",0,0,"");
                restart = true;
            } else if (event.id == MQTT_EVENT_PUBLISHED) {
                if (!ack_cached_record(event.msg_id)) {
                    heartbeat_ack(&heartbeat, event.msg_id);
                }
            }
        }

        recover_flash_cache();
        finish_flash_ack();
        int64_t now = now_ms();
        /* Continue collecting snapshots while offline after worker startup. */
        if (now >= next_upload) {
            if (cache_environment(device_id) < 0) {
                ESP_LOGW(TAG, "Snapshot not fully cached; keeping pending records");
            }
            next_upload = now_ms() + UPLOAD_INTERVAL_MS;
        }
        if (active_record.used && !active_record.ack_received && now >= active_record.deadline) {
            log_delivery(STORAGE_WARN,"replay_puback_timeout",ESP_ERR_TIMEOUT,
                         active_record.msg_id,active_record.record_id,active_record.wire_id);
            restart = true;
        }
        if (started && !connected && now >= connect_deadline) {
            storage_log(STORAGE_WARN,"mqtt","connect_timeout",ESP_ERR_TIMEOUT,0,"");
            restart = true;
        }
        if (connected && !restart) {
            pump_cached_records(client, device_id);
        }
        if (connected && !restart && now >= next_heartbeat) {
            restart = heartbeat_expire(&heartbeat);
            if (!restart) {
                int id = esp_mqtt_client_enqueue(client, CONFIG_INGAWAY_MQTT_TOPIC_HEARTBEAT,
                                                 "alive", 5, 1, 0, true);
                restart = heartbeat_sent(&heartbeat, id);
            }
            if (restart) storage_log(STORAGE_WARN,"mqtt","heartbeat_failed",ESP_ERR_TIMEOUT,0,"");
            next_heartbeat = now_ms() + HEARTBEAT_INTERVAL_MS;
        }

        if (restart) {
            connected = false;
            if (started) {
                esp_err_t err = esp_mqtt_client_stop(client);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "MQTT stop failed: %s; restart required", esp_err_to_name(err));
                    storage_log(STORAGE_ERROR,"mqtt","stop_failed",err,0,"restart required");
                    vTaskDelete(NULL);
                    return;
                }
                started = false;
            }
            /* Never run RAM outbox replay and cache replay in parallel.
             * Stop/join then destroy removes all old-session packet IDs. */
            if (client != NULL) {
                esp_err_t err = esp_mqtt_client_destroy(client);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "MQTT destroy failed: %s", esp_err_to_name(err));
                    storage_log(STORAGE_ERROR,"mqtt","destroy_failed",err,0,"");
                    vTaskDelete(NULL);
                    return;
                }
                client = NULL;
            }
            /* All old-session callbacks have finished before queue reset. */
            xQueueReset(events);
            take_overflow();
            heartbeat_reset(&heartbeat);
            reset_inflight_after_disconnect();
            next_connect = now_ms() + RECONNECT_INTERVAL_MS;
            ESP_LOGW(TAG, "MQTT offline; retry when Wi-Fi is ready (10 s cooldown)");
            storage_log(STORAGE_WARN,"mqtt","reconnect_scheduled",0,0,"10 second cooldown");
        }

        ready = get_network(&generation);
        now = now_ms();
        if (!started && ready && now >= next_connect) {
            /* Initialize SNTP even if retained wall clock already passes TLS's
             * plausibility gate; archival UTC requires an explicit sync anchor. */
            if (!sntp_initialized) {
                esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_INGAWAY_MQTT_NTP_SERVER);
                config.sync_cb = storage_clock_synced;
                esp_err_t err = esp_netif_sntp_init(&config);
                sntp_initialized = err == ESP_OK;
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
                    storage_log(STORAGE_WARN,"time","sntp_init_failed",err,0,"");
                    next_connect = now + RECONNECT_INTERVAL_MS;
                }
            }
            if (!clock_ready()) {
                if (now >= next_clock_notice) {
                    ESP_LOGW(TAG, "Waiting for time synchronization before TLS");
                    storage_log(STORAGE_WARN,"time","tls_waiting_for_clock",0,0,"");
                    next_clock_notice = now + 30000;
                }
            } else {
                if (client == NULL) {
                    client = esp_mqtt_client_init(&mqtt_cfg);
                    if (client != NULL && esp_mqtt_client_register_event(
                            client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL) != ESP_OK) {
                        esp_mqtt_client_destroy(client);
                        client = NULL;
                    }
                }
                if (client != NULL && esp_mqtt_client_start(client) == ESP_OK) {
                    started = true;
                    active_generation = generation;
                    connect_deadline = now_ms() + CONNECT_DEADLINE_MS;
                } else {
                    ESP_LOGW(TAG, "MQTT start failed; retry in 10 seconds");
                    storage_log(STORAGE_WARN,"mqtt","start_failed",ESP_FAIL,0,"");
                    next_connect = now_ms() + RECONNECT_INTERVAL_MS;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(WORKER_TICK_MS));
    }
}

void mqtt_network_down(void)
{
    portENTER_CRITICAL(&state_lock);
    network_ready = false;
    ++network_generation;
    portEXIT_CRITICAL(&state_lock);
}

/* Called on GOT_IP. Repeated calls neither duplicate tasks nor bypass cooldown. */
void mqtt_start(void)
{
    if (strncmp(CONFIG_INGAWAY_MQTT_BROKER_URI, "mqtts://", 8) != 0) {
        ESP_LOGE(TAG, "Broker URI must use mqtts://");
        return;
    }
    portENTER_CRITICAL(&state_lock);
    network_ready = true;
    portEXIT_CRITICAL(&state_lock);
    bool expected = false;
    if (!atomic_compare_exchange_strong(&worker_requested, &expected, true)) {
        return;
    }
    events = xQueueCreate(32, sizeof(connection_event_t));
    if (!events) {
        atomic_store(&worker_requested, false);
        ESP_LOGE(TAG, "No memory for MQTT events");
        return;
    }
    if (xTaskCreate(mqtt_worker, "mqtt_worker", 8192, NULL, 5, NULL) == pdPASS) {
        return;
    }
    vQueueDelete(events);
    events = NULL;
    atomic_store(&worker_requested, false);
    ESP_LOGE(TAG, "Failed to create MQTT worker");
}
