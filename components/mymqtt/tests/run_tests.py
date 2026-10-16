#!/usr/bin/env python3
"""Host tests of the real MQTT worker with deterministic RTOS/MQTT mocks."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
api = r'''
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define CONFIG_INGAWAY_MQTT_BROKER_URI "mqtts://10.30.192.176"
#define CONFIG_INGAWAY_MQTT_BROKER_PORT 8883
#define CONFIG_INGAWAY_MQTT_TOPIC_DATA "env/telemetry"
#define CONFIG_INGAWAY_MQTT_TOPIC_TEMP "env/temp"
#define CONFIG_INGAWAY_MQTT_TOPIC_HEARTBEAT "env/heartbeat"
#define CONFIG_INGAWAY_MQTT_NTP_SERVER "pool.ntp.org"
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define pdPASS 1
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_EVENT_ANY_ID -1
#define ESP_MAC_WIFI_STA 0
#define MQTT_EVENT_CONNECTED 1
#define MQTT_EVENT_DISCONNECTED 2
#define MQTT_EVENT_PUBLISHED 3
#define MQTT_EVENT_ERROR 4
typedef int esp_err_t;
typedef void *esp_event_base_t;
typedef void *esp_mqtt_client_handle_t;
typedef void *QueueHandle_t;
typedef struct {int unused;} esp_netif_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static inline void mock_log(const char *tag, const char *fmt, ...) {(void)tag; (void)fmt;}
#define ESP_LOGI(...) mock_log(__VA_ARGS__)
#define ESP_LOGW(...) mock_log(__VA_ARGS__)
#define ESP_LOGE(...) mock_log(__VA_ARGS__)
static inline const char *esp_err_to_name(int err) {(void)err; return "mock error";}
typedef struct { int error_type, esp_tls_last_esp_err, esp_tls_stack_err; } mock_error_t;
typedef struct { int msg_id; mock_error_t *error_handle; } esp_mqtt_event_t;
typedef esp_mqtt_event_t *esp_mqtt_event_handle_t;
typedef void (*event_handler_t)(void *, esp_event_base_t, int32_t, void *);
typedef struct {
    struct { struct {const char *uri; int port;} address;
             struct {const char *certificate; bool skip_cert_common_name_check;} verification; } broker;
    struct {struct {const char *certificate, *key;} authentication;} credentials;
    struct {int keepalive;} session;
    struct {bool disable_auto_reconnect; int timeout_ms;} network;
    struct {int limit;} outbox;
} esp_mqtt_client_config_t;
typedef struct {const char *server;} esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(s) { .server = (s) }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *);
esp_err_t esp_read_mac(uint8_t *, int);
int64_t esp_timer_get_time(void);
QueueHandle_t xQueueCreate(unsigned, size_t);
void vQueueDelete(QueueHandle_t);
int xQueueSend(QueueHandle_t, const void *, unsigned);
int xQueueReceive(QueueHandle_t, void *, unsigned);
void xQueueReset(QueueHandle_t);
void vTaskDelay(uint32_t);
void vTaskDelete(void *);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *);
esp_err_t esp_mqtt_client_register_event(void *, int, event_handler_t, void *);
esp_err_t esp_mqtt_client_destroy(void *);
esp_err_t esp_mqtt_client_start(void *);
esp_err_t esp_mqtt_client_stop(void *);
int esp_mqtt_client_enqueue(void *, const char *, const char *, int, int, int, bool);
int w25q64_cache_init(void);
int w25q64_cache_append(const void *, size_t, uint64_t *);
int w25q64_cache_peek(void *, size_t, size_t *, uint64_t *);
int w25q64_cache_ack(uint64_t);
'''
with tempfile.TemporaryDirectory(prefix="ingaway-mqtt-tests-") as tmp:
    root = Path(tmp)
    (root / "host_api.h").write_text(api)
    for name in ["freertos/FreeRTOS.h", "freertos/task.h", "freertos/queue.h", "esp_log.h",
                 "esp_timer.h", "esp_err.h", "esp_netif_types.h", "mqtt_client.h", "esp_mac.h",
                 "esp_netif_sntp.h"]:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "host_api.h"\n')
    for test in ["test_heartbeat.c", "test_worker.c"]:
        exe = root / test.removesuffix('.c')
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                        "-I", str(root), "-I", str(here.parent / "include"),
                        "-I", str(here.parent.parent / "modbusTCP/include"),
                        "-I", str(here.parent.parent / "w25q64/include"),
                        str(here / test), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
