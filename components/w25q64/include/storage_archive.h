#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/time.h>
#include "esp_err.h"
#include "flash_fifo.h"

/* Separate archival rings; never use MQTT PUBACK to consume these records. */
typedef enum { STORAGE_DEBUG, STORAGE_INFO, STORAGE_WARN, STORAGE_ERROR } storage_level_t;
typedef enum { STORAGE_HISTORY, STORAGE_LOGS } storage_archive_kind_t;
typedef struct {
    uint16_t value;
    uint8_t type;
    bool has_value, stale;
    int64_t last_success_ms;
    int32_t state, error;
} storage_sample_t;
typedef struct {
    uint32_t history_dropped, logs_dropped;
    uint32_t history_evicted, logs_evicted;
    uint32_t history_write_errors, logs_write_errors;
    bool history_ready, logs_ready;
} storage_archive_stats_t;
/* Call after w25q64_init, before Wi-Fi/tasks start. Creates one storage task.
 * Submit functions are task-context, nonblocking, copy all data; no Flash IO.
 * ESP_OK means queued, not durable; queue overflow returns ESP_ERR_NO_MEM. */
esp_err_t storage_archive_start(void);
esp_err_t storage_history_submit(const storage_sample_t *sample);
esp_err_t storage_log(storage_level_t level, const char *source, const char *event,
                      int32_t error, uint64_t record_id, const char *detail);
/* SNTP notification hook. UTC is valid only after this explicit sync event.
 * Writes no Flash and may be called from the SNTP task. */
void storage_clock_synced(struct timeval *tv);
/* Blocking task-context read, serialized with the writer. Returns FF_* codes.
 * Copies one JSON record without consuming it; after=0 starts at oldest.
 * len excludes NUL; caller must provide one extra byte to terminate itself. */
int storage_archive_read(storage_archive_kind_t kind, uint64_t after,
                         void *data, size_t capacity, size_t *len, uint64_t *id);
void storage_archive_get_stats(storage_archive_stats_t *stats);
