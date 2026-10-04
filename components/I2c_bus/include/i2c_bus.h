#pragma once

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create one bus and pass it to both sensor drivers. Use external pull-ups. */
esp_err_t i2c_bus_init(i2c_port_num_t port, gpio_num_t sda, gpio_num_t scl,
                       i2c_master_bus_handle_t *out_bus);
/* Remove all sensor devices before deleting the bus. */
esp_err_t i2c_bus_deinit(i2c_master_bus_handle_t bus);

#ifdef __cplusplus
}
#endif
