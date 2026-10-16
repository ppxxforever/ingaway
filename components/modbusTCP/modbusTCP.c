#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "modbusTCP.h"
#include "mbcontroller.h"
#include "mb_types.h"
#include "storage_archive.h"

#define TAG "modbusTCP"
#define MB_TCP_PORT 502
#define MB_WORKER_PRIORITY (CONFIG_FMB_PORT_TASK_PRIO - 1)
#define WAIT_SLICE_MS 100
#define RESOURCE_LOG_INTERVAL_MS 60000
#define INTERNAL_HEAP_WARN_BYTES 32768
#define STACK_WARN_BYTES 1024

/* esp-modbus 2.1.4 acknowledges stop before suspending its TCP task.
 * Keep lifecycle calls on the driver's core, at a lower priority.
 */
_Static_assert(CONFIG_FMB_PORT_TASK_PRIO > 1, "Modbus driver priority must exceed worker priority");
_Static_assert(CONFIG_FMB_PORT_TASK_AFFINITY == 0 || CONFIG_FMB_PORT_TASK_AFFINITY == 1,
               "Pin the Modbus driver to a CPU for safe initialization");
_Static_assert(CONFIG_INGAWAY_MB_RESPONSE_TIMEOUT_MS < CONFIG_FMB_MASTER_MAX_API_BLOCKING_TIME_MS,
               "Response timeout must be below the library API blocking limit");
_Static_assert(CONFIG_INGAWAY_MB_BACKOFF_MAX_MS >= CONFIG_INGAWAY_MB_BACKOFF_MIN_MS,
               "Maximum backoff must be at least the initial backoff");

enum { CID_TEMP = 0, CID_HMI = 1, CID_PRESSURE = 2 };
static void *master_handle;
static atomic_bool worker_requested;
static portMUX_TYPE sample_lock = portMUX_INITIALIZER_UNLOCKED;
static bool network_ready;
static uint32_t network_generation;
static modbus_temp_sample_t latest = {
    .stale = true,
    .last_error = ESP_ERR_INVALID_STATE,
    .state = MODBUS_WAIT_NETWORK,
};
static char *slaveIP_table[] = { "1;10.30.192.176;502", NULL };
static const mb_parameter_descriptor_t device_parameters[] = {
    {CID_TEMP, "TEMP VALUE", "C", 1, MB_PARAM_HOLDING, 0, 1, 0,
     PARAM_TYPE_U16, 2, {.opt1 = 0, .opt2 = 0, .opt3 = 0}, PAR_PERMS_READ},
};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool network_snapshot(uint32_t *generation)
{
    portENTER_CRITICAL(&sample_lock);
    bool ready = network_ready;
    *generation = network_generation;
    portEXIT_CRITICAL(&sample_lock);
    return ready;
}

static bool same_network(uint32_t generation)
{
    uint32_t current;
    return network_snapshot(&current) && current == generation;
}

static void archive_snapshot(const modbus_temp_sample_t *sample)
{
    storage_sample_t record={.value=sample->value,.type=sample->type,
        .has_value=sample->has_value,.stale=sample->stale,
        .last_success_ms=sample->last_success_ms,.state=sample->state,.error=sample->last_error};
    storage_history_submit(&record);
}
static void archive_sample(void)
{
    modbus_temp_sample_t sample;
    if (modbusTCP_get_temp_sample(&sample) == ESP_OK) archive_snapshot(&sample);
}

static void mark_stale(esp_err_t err, modbus_acquisition_state_t state)
{
    portENTER_CRITICAL(&sample_lock);
    latest.stale = true;
    latest.last_error = err;
    latest.state = state;
    portEXIT_CRITICAL(&sample_lock);
}

static void set_state(modbus_acquisition_state_t state)
{
    portENTER_CRITICAL(&sample_lock);
    latest.state = state;
    portEXIT_CRITICAL(&sample_lock);
}

/* Publish only if no Wi-Fi disconnect occurred while the request was in flight. */
static bool publish_temp(uint16_t value, uint8_t type, uint32_t generation)
{
    int64_t timestamp = now_ms();
    modbus_temp_sample_t snapshot;
    portENTER_CRITICAL(&sample_lock);
    bool accepted = network_ready && generation == network_generation;
    if (accepted) {
        latest.value = value;
        latest.type = type;
        latest.has_value = true;
        latest.stale = false;
        latest.last_success_ms = timestamp;
        latest.last_error = ESP_OK;
        latest.state = MODBUS_POLLING;
        snapshot=latest;
    }
    portEXIT_CRITICAL(&sample_lock);
    if (accepted) archive_snapshot(&snapshot);
    return accepted;
}

void modbusTCP_network_down(void)
{
    portENTER_CRITICAL(&sample_lock);
    network_ready = false;
    ++network_generation;
    latest.stale = true;
    latest.last_error = ESP_ERR_INVALID_STATE;
    latest.state = MODBUS_WAIT_NETWORK;
    portEXIT_CRITICAL(&sample_lock);
    archive_sample();
    storage_log(STORAGE_WARN,"modbus","network_down",ESP_ERR_INVALID_STATE,0,"sample marked stale");
}

/* Normal polling/retry waits abort promptly on a network change. Backoff does
 * not: repeated GOT_IP events must not bypass the reconnect rate limit.
 */
static bool wait_on_network(uint32_t delay_ms, uint32_t generation)
{
    int64_t deadline = now_ms() + delay_ms;
    while (same_network(generation)) {
        esp_task_wdt_reset();
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0) {
            return true;
        }
        uint32_t slice = remaining < WAIT_SLICE_MS ? (uint32_t)remaining : WAIT_SLICE_MS;
        vTaskDelay(pdMS_TO_TICKS(slice) + 1);
    }
    return false;
}

/* Only the worker touches controller ownership or performs network requests. */
static esp_err_t connect_master(esp_netif_t *netif)
{
    mb_communication_info_t config = {
        .tcp_opts.port = MB_TCP_PORT,
        .tcp_opts.mode = MB_TCP,
        .tcp_opts.addr_type = MB_IPV4,
        .tcp_opts.ip_addr_table = (void *)slaveIP_table,
        .tcp_opts.uid = 0,
        /* This wait is bounded by CONFIG_FMB_TCP_CONNECTION_TOUT_SEC. */
        .tcp_opts.start_disconnected = false,
        .tcp_opts.response_tout_ms = CONFIG_INGAWAY_MB_RESPONSE_TIMEOUT_MS,
        .tcp_opts.ip_netif_ptr = netif,
    };
    esp_err_t err = mbc_master_create_tcp(&config, &master_handle);
    if (err == ESP_OK) {
        err = mbc_master_set_descriptor(master_handle, device_parameters,
                                       sizeof(device_parameters) / sizeof(device_parameters[0]));
    }
    if (err == ESP_OK) {
        err = mbc_master_start(master_handle);
    }
    return err;
}

static void log_resources(const char *event)
{
    char detail[96];
    UBaseType_t stack_free = uxTaskGetStackHighWaterMark(NULL);
    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    snprintf(detail, sizeof(detail), "heap=%u min=%u stack_hwm=%u",
             (unsigned)heap_free, (unsigned)heap_min, (unsigned)stack_free);
    storage_level_t level = (heap_free < INTERNAL_HEAP_WARN_BYTES || stack_free < STACK_WARN_BYTES)
                            ? STORAGE_WARN : STORAGE_DEBUG;
    storage_log(level, "modbus", event, 0, stack_free, detail);
    if (level == STORAGE_WARN) ESP_LOGW(TAG, "%s: %s", event, detail);
    else ESP_LOGI(TAG, "%s: %s", event, detail);
}

static bool release_master(void)
{
    if (master_handle == NULL) {
        return true;
    }
    esp_err_t err = mbc_master_delete(master_handle);
    if (err != ESP_OK) {
        /* Public delete may have partially freed the controller. Never reuse it. */
        mark_stale(err, MODBUS_FAULT);
        ESP_LOGE(TAG, "Controller cleanup failed: %s; restart required", esp_err_to_name(err));
        archive_sample();
        storage_log(STORAGE_ERROR,"modbus","controller_cleanup_failed",err,0,"restart required");
        return false;
    }
    master_handle = NULL;
    return true;
}

static void modbus_poll_task(void *arg)
{
    esp_netif_t *netif = arg;
    uint32_t backoff_ms = CONFIG_INGAWAY_MB_BACKOFF_MIN_MS;
    bool acquisition_healthy=false;
    int64_t next_resource_log = 0;
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK && wdt_err != ESP_ERR_INVALID_STATE) {
        storage_log(STORAGE_WARN,"modbus","watchdog_add_failed",wdt_err,0,"");
    }
    log_resources("worker_start");
    for (;;) {
        esp_task_wdt_reset();
        if (now_ms() >= next_resource_log) {
            log_resources("resource_check");
            next_resource_log = now_ms() + RESOURCE_LOG_INTERVAL_MS;
        }
        uint32_t generation;
        if (!network_snapshot(&generation)) {
            mark_stale(ESP_ERR_INVALID_STATE, MODBUS_WAIT_NETWORK);
            if (!release_master()) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(WAIT_SLICE_MS) + 1);
            continue;
        }

        bool success = false;
        esp_err_t err = ESP_ERR_INVALID_STATE;
        for (unsigned attempt = 0; attempt <= CONFIG_INGAWAY_MB_MAX_RETRIES; ++attempt) {
            if (!same_network(generation)) {
                break;
            }
            if (master_handle == NULL) {
                set_state(MODBUS_CONNECTING);
                storage_log(STORAGE_DEBUG,"modbus","connect_attempt",0,attempt,"");
                err = connect_master(netif);
                if (err != ESP_OK) {
                    storage_log(STORAGE_WARN,"modbus","connect_failed",err,attempt,"");
                    mark_stale(err, MODBUS_CONNECTING);
                    if (!release_master()) {
                        goto fault;
                    }
                }
            } else {
                err = ESP_OK;
            }
            if (err == ESP_OK && same_network(generation)) {
                uint16_t value = 0;
                uint8_t type = 0;
                /* Library owns MBAP, transaction IDs, byte order and TCP framing. */
                err = mbc_master_get_parameter(master_handle, CID_TEMP, (uint8_t *)&value, &type);
                if (err == ESP_OK && type != PARAM_TYPE_U16) {
                    err = ESP_ERR_INVALID_RESPONSE;
                }
                if (err == ESP_OK && publish_temp(value, type, generation)) {
                    ESP_LOGI(TAG, "TEMP raw=%" PRIu16, value);
                    if (!acquisition_healthy) {
                        storage_log(STORAGE_INFO,"modbus","acquisition_recovered",0,0,"");
                        acquisition_healthy=true;
                    }
                    success = true;
                    break;
                }
                if (err == ESP_OK) {
                    err = ESP_ERR_INVALID_STATE;
                }
                mark_stale(err, MODBUS_CONNECTING);
                storage_log(STORAGE_WARN,"modbus","read_failed_releasing",err,attempt,
                            "close socket/controller before retry");
                if (!release_master()) {
                    goto fault;
                }
            }
            esp_task_wdt_reset();
            if (attempt < CONFIG_INGAWAY_MB_MAX_RETRIES &&
                !wait_on_network(CONFIG_INGAWAY_MB_RETRY_DELAY_MS, generation)) {
                break;
            }
        }

        if (success) {
            backoff_ms = CONFIG_INGAWAY_MB_BACKOFF_MIN_MS;
            if (!wait_on_network(CONFIG_INGAWAY_MB_POLL_INTERVAL_MS, generation)) {
                /* Even a very brief disconnect invalidates the old TCP session. */
                if (!release_master()) {
                    break;
                }
            }
            continue;
        }

        mark_stale(err, MODBUS_BACKOFF);
        acquisition_healthy=false;
        archive_sample();
        storage_log(STORAGE_WARN,"modbus","acquisition_failed",err,backoff_ms,
                    "record_id is reconnect backoff milliseconds");
        /* Release before sleeping: no hidden library reconnect loop in backoff. */
        if (!release_master()) {
            break;
        }
        ESP_LOGW(TAG, "Acquisition failed: %s; reconnect in %" PRIu32 " ms",
                 esp_err_to_name(err), backoff_ms);
        int64_t backoff_deadline = now_ms() + backoff_ms;
        while (now_ms() < backoff_deadline) {
            esp_task_wdt_reset();
            int64_t remaining = backoff_deadline - now_ms();
            uint32_t slice = remaining < WAIT_SLICE_MS ? (uint32_t)remaining : WAIT_SLICE_MS;
            vTaskDelay(pdMS_TO_TICKS(slice) + 1);
        }
        backoff_ms = backoff_ms > CONFIG_INGAWAY_MB_BACKOFF_MAX_MS / 2
                     ? CONFIG_INGAWAY_MB_BACKOFF_MAX_MS : backoff_ms * 2;
    }
fault:
    /* Keep worker_requested set: do not duplicate a partially destroyed stack. */
    log_resources("worker_fault");
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

int modbusTCP_init(esp_netif_t *netif)
{
    if (netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&sample_lock);
    network_ready = true;
    ++network_generation;
    portEXIT_CRITICAL(&sample_lock);
    bool expected = false;
    if (!atomic_compare_exchange_strong(&worker_requested, &expected, true)) {
        return ESP_OK;
    }
    if (xTaskCreatePinnedToCore(modbus_poll_task, "modbus_poll", 4096, netif,
                               MB_WORKER_PRIORITY, NULL, CONFIG_FMB_PORT_TASK_AFFINITY) != pdPASS) {
        atomic_store(&worker_requested, false);
        mark_stale(ESP_ERR_NO_MEM, MODBUS_FAULT);
        storage_log(STORAGE_ERROR,"modbus","worker_create_failed",ESP_ERR_NO_MEM,0,"");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t modbusTCP_get_temp_sample(modbus_temp_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t timestamp = now_ms();
    portENTER_CRITICAL(&sample_lock);
    *sample = latest;
    sample->stale = sample->stale || !network_ready || !sample->has_value ||
                    timestamp - sample->last_success_ms >= CONFIG_INGAWAY_MB_STALE_AFTER_MS;
    portEXIT_CRITICAL(&sample_lock);
    return ESP_OK;
}

/* Compatibility accessor: reads the cache, never performs a second transaction.
 * Leave the caller's buffer unchanged if no fresh sample exists.
 */
int read_temp(uint8_t *temp, uint8_t *type)
{
    if (temp == NULL || type == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    modbus_temp_sample_t sample;
    modbusTCP_get_temp_sample(&sample);
    if (!sample.has_value) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample.stale) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(temp, &sample.value, sizeof(sample.value));
    *type = sample.type;
    return ESP_OK;
}

int read_hmi(uint8_t *value, uint8_t *type)
{
    if (value == NULL || type == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* CID_HMI has no register descriptor yet; do not invent its mapping. */
    return ESP_ERR_NOT_SUPPORTED;
}
