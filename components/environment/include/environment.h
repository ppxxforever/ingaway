#pragma once
#include "esp_err.h"
#include "bmp280.h"
#include "aht20.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Call once from app_main. Starts a dedicated sensor sampling task. */
esp_err_t environment_start(void);
extern bmp280_handle_t bmp;
extern aht20_handle_t aht;
#ifdef __cplusplus
}
#endif
