#include "i2c_bus.h"

esp_err_t i2c_bus_init(i2c_port_num_t port, gpio_num_t sda, gpio_num_t scl,
                       i2c_master_bus_handle_t *out_bus)
{
    if (!out_bus || sda == scl) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_bus = NULL;
    const i2c_master_bus_config_t config = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&config, out_bus);
}

esp_err_t i2c_bus_deinit(i2c_master_bus_handle_t bus)
{
    return bus ? i2c_del_master_bus(bus) : ESP_ERR_INVALID_ARG;
}
