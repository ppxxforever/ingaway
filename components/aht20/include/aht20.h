#pragma once
#include "i2c_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aht20 *aht20_handle_t;

/* AHT20 has fixed I2C address 0x38. */
esp_err_t aht20_init(i2c_master_bus_handle_t bus, aht20_handle_t *out_sensor);
/* Blocking measurement; serialize calls using the same sensor handle. */
esp_err_t aht20_read(aht20_handle_t sensor, float *temperature_c, float *humidity_percent);
esp_err_t aht20_deinit(aht20_handle_t sensor);

#ifdef __cplusplus
}
#endif
