#pragma once
#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call once at startup. Detect/register only; never erases or formats data. */
esp_err_t w25q64_init(void);
/* Return NULL before successful initialization. Handles live for app lifetime. */
const esp_partition_t *w25q64_history_partition(void);
const esp_partition_t *w25q64_cache_partition(void);
const esp_partition_t *w25q64_logs_partition(void);

#ifdef __cplusplus
}
#endif
