#pragma once
#include "flash_fifo.h"
#include "esp_err.h"
/* Initialize after w25q64_init. No erase at open. Single storage-task owner;
 * not thread-safe. IDs are unique within this chip/format (pair with device ID).
 * Ack only after application delivery confirmation, e.g. QoS1 PUBACK. */
esp_err_t w25q64_cache_init(void);
int w25q64_cache_append(const void *data, size_t len, uint64_t *id);
int w25q64_cache_peek(void *data, size_t capacity, size_t *len, uint64_t *id);
int w25q64_cache_ack(uint64_t id);
