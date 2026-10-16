#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_SERVICE_IDLE = 0,
    OTA_SERVICE_CHECKING,
    OTA_SERVICE_SKIPPED,
    OTA_SERVICE_DOWNLOADING,
    OTA_SERVICE_UPDATED,
    OTA_SERVICE_FAILED,
} ota_service_state_t;

typedef struct {
    ota_service_state_t state;
    int progress_percent;
    esp_err_t last_error;
    char current_version[32];
    char candidate_version[32];
    char detail[96];
} ota_service_status_t;

esp_err_t ota_service_start_once(void);
ota_service_status_t ota_service_get_status(void);

#ifdef __cplusplus
}
#endif
