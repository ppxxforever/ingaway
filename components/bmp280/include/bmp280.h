#pragma once
#include "i2c_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bmp280 *bmp280_handle_t;

/* Address is 0x76 (SDO low) or 0x77 (SDO high). CSB must be high for I2C. */
esp_err_t bmp280_init(i2c_master_bus_handle_t bus, uint8_t address,
                      bmp280_handle_t *out_sensor);
/* Blocking measurement; serialize calls using the same sensor handle. */
esp_err_t bmp280_read(bmp280_handle_t sensor, float *temperature_c, float *pressure_hpa);
esp_err_t bmp280_deinit(bmp280_handle_t sensor);

#ifdef __cplusplus
}
#endif
