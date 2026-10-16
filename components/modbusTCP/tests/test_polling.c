#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "../modbusTCP.c"

static jmp_buf done;
static esp_netif_t netif;
static int64_t clock_ms;
static unsigned creates, starts, deletes, reads, tasks, pauses, wdt_resets;
static uint32_t pause_ms[16];
static uint32_t pause_accum;
static int scenario;
static bool create_task_fails;
static void (*worker)(void *);
static void *worker_arg;

enum { CONNECT_FAIL, READ_RECOVER, WIFI_FLAP, WIFI_RECOVER, CLEANUP_FAIL, BACKOFF_RESET, GOOD };

int64_t esp_timer_get_time(void) { return clock_ms * 1000; }
int xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, unsigned stack,
                            void *arg, unsigned priority, void *handle, int core)
{
    (void)name; (void)stack; (void)handle;
    assert(core == CONFIG_FMB_PORT_TASK_AFFINITY);
    assert(priority < CONFIG_FMB_PORT_TASK_PRIO);
    if (create_task_fails) return 0;
    ++tasks; worker = fn; worker_arg = arg;
    return pdPASS;
}
void vTaskDelete(void *task) { assert(task == NULL); longjmp(done, 2); }
UBaseType_t uxTaskGetStackHighWaterMark(void *task) { (void)task; return 2048; }
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 128 * 1024; }
size_t heap_caps_get_minimum_free_size(unsigned caps) { (void)caps; return 96 * 1024; }
esp_err_t esp_task_wdt_add(void *task) { assert(task == NULL); return ESP_OK; }
esp_err_t esp_task_wdt_reset(void) { ++wdt_resets; return ESP_OK; }
esp_err_t esp_task_wdt_delete(void *task) { assert(task == NULL); return ESP_OK; }
esp_err_t mbc_master_create_tcp(mb_communication_info_t *config, void **handle)
{
    assert(master_handle == NULL);
    assert(!config->tcp_opts.start_disconnected);
    assert(config->tcp_opts.response_tout_ms == 2000);
    assert(config->tcp_opts.ip_netif_ptr == &netif);
    ++creates;
    *handle = (void *)&netif;
    return ESP_OK;
}
esp_err_t mbc_master_set_descriptor(void *handle, const mb_parameter_descriptor_t *desc, unsigned count)
{
    assert(handle != NULL && count == 1 && desc[0].size == 2);
    return ESP_OK;
}
esp_err_t mbc_master_start(void *handle)
{
    assert(handle != NULL);
    ++starts;
    if (scenario == CONNECT_FAIL) {
        clock_ms += CONFIG_FMB_TCP_CONNECTION_TOUT_SEC * 1000;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
esp_err_t mbc_master_delete(void *handle)
{
    assert(handle != NULL);
    ++deletes;
    return scenario == CLEANUP_FAIL ? ESP_ERR_INVALID_STATE : ESP_OK;
}
esp_err_t mbc_master_get_parameter(void *handle, int cid, uint8_t *value, uint8_t *type)
{
    assert(handle != NULL && cid == CID_TEMP);
    ++reads;
    if ((scenario == READ_RECOVER && reads <= 3) || scenario == CLEANUP_FAIL ||
        (scenario == BACKOFF_RESET && reads != 7)) {
        clock_ms += 2000;
        return ESP_ERR_TIMEOUT;
    }
    if (scenario == WIFI_FLAP && reads == 1) {
        modbusTCP_network_down();
        assert(modbusTCP_init(&netif) == ESP_OK);
        assert(tasks == 1);
    }
    uint16_t raw = (uint16_t)(250 + reads);
    memcpy(value, &raw, sizeof(raw));
    *type = PARAM_TYPE_U16;
    return ESP_OK;
}
void vTaskDelay(uint32_t ticks)
{
    clock_ms += ticks;
    assert(clock_ms < 1000000); /* No unbounded loop in any scenario. */
    if (scenario == WIFI_RECOVER && !network_ready) {
        assert(creates == 0 && reads == 0);
        assert(modbusTCP_init(&netif) == ESP_OK);
        return;
    }
    if (scenario == WIFI_FLAP && reads == 1) {
        assert(!latest.has_value && latest.stale); /* Old-session response discarded. */
    }
    if ((scenario == CONNECT_FAIL || scenario == READ_RECOVER || scenario == BACKOFF_RESET) &&
        latest.state == MODBUS_BACKOFF) {
        static const uint32_t connect_expected[] = {1000,2000,4000,8000,16000,30000,30000,30000};
        static const uint32_t reset_expected[] = {1000,2000,1000};
        const uint32_t *expected = scenario == CONNECT_FAIL ? connect_expected : reset_expected;
        assert(master_handle == NULL);
        pause_accum += ticks - 1;
        if (pause_accum >= expected[pauses]) {
            pause_ms[pauses] = expected[pauses];
            ++pauses;
            pause_accum = 0;
        }
        if (scenario == CONNECT_FAIL) {
            assert(deletes == starts);
            if (pauses == 8) longjmp(done, 1);
        } else if (scenario == BACKOFF_RESET) {
            if (pauses == 3) longjmp(done, 1);
        } else {
            assert(reads == 3 && deletes == 3);
            assert(latest.stale && latest.has_value && latest.value == 123);
        }
    }
    if (latest.state == MODBUS_POLLING) {
        assert(latest.has_value && !latest.stale && latest.last_error == ESP_OK);
        if (scenario == BACKOFF_RESET) return;
        if (scenario == READ_RECOVER) assert(reads == 4 && creates >= 4);
        if (scenario == WIFI_FLAP) assert(reads == 2 && creates == 2);
        longjmp(done, 1);
    }
}

static void reset(int next_scenario)
{
    scenario = next_scenario;
    clock_ms = creates = starts = deletes = reads = tasks = pauses = wdt_resets = pause_accum = 0;
    create_task_fails = false;
    master_handle = NULL;
    atomic_store(&worker_requested, false);
    network_ready = false;
    network_generation = 0;
    latest = (modbus_temp_sample_t){.stale = true, .last_error = ESP_ERR_INVALID_STATE,
                                  .state = MODBUS_WAIT_NETWORK};
}
static void run(void)
{
    assert(worker != NULL);
    if (setjmp(done) == 0) worker(worker_arg);
}

int main(void)
{
    reset(CONNECT_FAIL);
    assert(modbusTCP_init(&netif) == ESP_OK);
    assert(modbusTCP_init(&netif) == ESP_OK && tasks == 1);
    run();
    uint32_t expected[] = {1000,2000,4000,8000,16000,30000,30000,30000};
    assert(pauses == 8 && memcmp(expected, pause_ms, sizeof(expected)) == 0);
    assert(reads == 0);

    reset(BACKOFF_RESET);
    assert(modbusTCP_init(&netif) == ESP_OK);
    run();
    assert(pauses == 3 && pause_ms[0] == 1000 && pause_ms[1] == 2000 && pause_ms[2] == 1000);
    assert(reads >= 7 && latest.has_value && latest.value == 257);

    reset(READ_RECOVER);
    assert(modbusTCP_init(&netif) == ESP_OK);
    assert(publish_temp(123, PARAM_TYPE_U16, network_generation));
    run();
    uint16_t output = 0;
    uint8_t type = 0;
    assert(read_temp((uint8_t *)&output, &type) == ESP_OK && output == 254 && reads == 4 && creates == 4);
    assert(type == PARAM_TYPE_U16);
    modbus_temp_sample_t sample;
    clock_ms = latest.last_success_ms + CONFIG_INGAWAY_MB_STALE_AFTER_MS;
    assert(modbusTCP_get_temp_sample(&sample) == ESP_OK && sample.stale && sample.value == 254);
    output = 999; type = 99;
    assert(read_temp((uint8_t *)&output, &type) == ESP_ERR_TIMEOUT && output == 999 && type == 99);
    modbusTCP_network_down();
    assert(latest.stale);

    reset(WIFI_FLAP);
    assert(modbusTCP_init(&netif) == ESP_OK);
    run();
    assert(tasks == 1 && deletes == 1 && latest.value == 252);

    reset(WIFI_RECOVER);
    assert(modbusTCP_init(&netif) == ESP_OK);
    modbusTCP_network_down();
    run();
    assert(tasks == 1 && reads == 1);

    reset(CLEANUP_FAIL);
    assert(modbusTCP_init(&netif) == ESP_OK);
    run();
    assert(latest.state == MODBUS_FAULT && latest.stale && reads == 1);
    assert(modbusTCP_init(&netif) == ESP_OK && tasks == 1);

    reset(GOOD);
    assert(read_temp((uint8_t *)&output, &type) == ESP_ERR_INVALID_STATE);
    assert(modbusTCP_get_temp_sample(NULL) == ESP_ERR_INVALID_ARG);
    assert(modbusTCP_init(NULL) == ESP_ERR_INVALID_ARG);
    create_task_fails = true;
    assert(modbusTCP_init(&netif) == ESP_ERR_NO_MEM);
    create_task_fails = false;
    assert(modbusTCP_init(&netif) == ESP_OK && tasks == 1);
    run();
    assert(read_hmi((uint8_t *)&output, &type) == ESP_ERR_NOT_SUPPORTED);
    puts("PASS: finite retries, capped backoff, stale cache, recovery, Wi-Fi epoch, singleton and failure paths");
    return 0;
}
