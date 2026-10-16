#!/usr/bin/env python3
"""Compile the actual acquisition component against deterministic host mocks."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
api = r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#define CONFIG_FMB_PORT_TASK_PRIO 10
#define CONFIG_FMB_PORT_TASK_AFFINITY 0
#define CONFIG_FMB_MASTER_MAX_API_BLOCKING_TIME_MS 6000
#define CONFIG_FMB_TCP_CONNECTION_TOUT_SEC 2
#define CONFIG_INGAWAY_MB_RESPONSE_TIMEOUT_MS 2000
#define CONFIG_INGAWAY_MB_POLL_INTERVAL_MS 2000
#define CONFIG_INGAWAY_MB_RETRY_DELAY_MS 250
#define CONFIG_INGAWAY_MB_MAX_RETRIES 2
#define CONFIG_INGAWAY_MB_BACKOFF_MIN_MS 1000
#define CONFIG_INGAWAY_MB_BACKOFF_MAX_MS 30000
#define CONFIG_INGAWAY_MB_STALE_AFTER_MS 5000
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define pdPASS 1
typedef enum { STORAGE_DEBUG, STORAGE_INFO, STORAGE_WARN, STORAGE_ERROR } storage_level_t;
typedef struct { uint16_t value; uint8_t type; bool has_value, stale; int64_t last_success_ms; int state; int error; } storage_sample_t;
static inline void storage_log(storage_level_t l,const char*c,const char*e,int err,uint64_t id,const char*d){(void)l;(void)c;(void)e;(void)err;(void)id;(void)d;}
static inline void storage_history_submit(const storage_sample_t *r){(void)r;}
#define pdMS_TO_TICKS(ms) (ms)
typedef int esp_err_t;
typedef struct { int unused; } esp_netif_t;
typedef int portMUX_TYPE;
typedef unsigned UBaseType_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static inline void mock_log(const char *tag, const char *fmt, ...) {(void)tag; (void)fmt;}
#define ESP_LOGI(...) mock_log(__VA_ARGS__)
#define ESP_LOGW(...) mock_log(__VA_ARGS__)
#define ESP_LOGE(...) mock_log(__VA_ARGS__)
static inline const char *esp_err_to_name(int err) {(void)err; return "mock error";}
int64_t esp_timer_get_time(void);
void vTaskDelay(uint32_t ticks);
void vTaskDelete(void *task);
UBaseType_t uxTaskGetStackHighWaterMark(void *task);
int xTaskCreatePinnedToCore(void (*fn)(void *), const char *, unsigned, void *, unsigned, void *, int);
#define MALLOC_CAP_INTERNAL 0
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_minimum_free_size(unsigned caps);
esp_err_t esp_task_wdt_add(void *task);
esp_err_t esp_task_wdt_reset(void);
esp_err_t esp_task_wdt_delete(void *task);
#define MB_TCP 1
#define MB_IPV4 0
#define MB_PARAM_HOLDING 0
#define PARAM_TYPE_U16 2
#define PAR_PERMS_READ 1
typedef struct { struct { int port, mode, addr_type; void *ip_addr_table; int uid;
    bool start_disconnected; unsigned response_tout_ms; esp_netif_t *ip_netif_ptr;
} tcp_opts; } mb_communication_info_t;
typedef struct { int cid; const char *key, *unit; int uid, reg_type, start, count, offset, type, size;
    struct {int opt1,opt2,opt3;} opts; int permissions;
} mb_parameter_descriptor_t;
esp_err_t mbc_master_create_tcp(mb_communication_info_t *, void **);
esp_err_t mbc_master_set_descriptor(void *, const mb_parameter_descriptor_t *, unsigned);
esp_err_t mbc_master_start(void *);
esp_err_t mbc_master_delete(void *);
esp_err_t mbc_master_get_parameter(void *, int, uint8_t *, uint8_t *);
'''
with tempfile.TemporaryDirectory(prefix="ingaway-modbus-tests-") as tmp:
    root = Path(tmp)
    (root / "host_api.h").write_text(api)
    for name in ["freertos/FreeRTOS.h", "freertos/task.h", "esp_heap_caps.h", "esp_log.h",
                 "esp_task_wdt.h", "esp_timer.h", "esp_err.h", "esp_netif_types.h",
                 "mbcontroller.h", "mb_types.h", "storage_archive.h"]:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "host_api.h"\n')
    exe = root / "test_polling"
    env = {**dict(__import__('os').environ), "PATH": "/usr/bin:/bin"}
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                    "-I", str(root), "-I", str(here.parent / "include"),
                    str(here / "test_polling.c"), "-o", str(exe)], check=True, env=env)
    subprocess.run([str(exe)], check=True)
