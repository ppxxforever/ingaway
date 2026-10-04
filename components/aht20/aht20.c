#include "aht20.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct aht20 {
    i2c_master_dev_handle_t dev;
};

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

static esp_err_t read_status(aht20_handle_t s, uint8_t *status)
{
    const uint8_t command = 0x71;
    return i2c_master_transmit_receive(s->dev, &command, 1, status, 1, 100);
}

static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t aht20_init(i2c_master_bus_handle_t bus, aht20_handle_t *out_sensor)
{
    if (!bus || !out_sensor) return ESP_ERR_INVALID_ARG;
    *out_sensor = NULL;
    aht20_handle_t s = calloc(1, sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x38,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &config, &s->dev);
    if (err != ESP_OK) { free(s); return err; }
    delay_ms(40); /* Power-on stabilization. */
    uint8_t status;
    err = read_status(s, &status);
    if (err != ESP_OK) goto fail;
    if (!(status & 0x08)) {
        const uint8_t init[] = {0xBE, 0x08, 0x00};
        err = i2c_master_transmit(s->dev, init, sizeof(init), 100);
        if (err != ESP_OK) goto fail;
        delay_ms(10);
        err = read_status(s, &status);
        if (err != ESP_OK) goto fail;
    }
    if (!(status & 0x08) || (status & 0x80)) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }
    *out_sensor = s;
    return ESP_OK;
fail:
    i2c_master_bus_rm_device(s->dev);
    free(s);
    return err;
}

esp_err_t aht20_read(aht20_handle_t s, float *temperature_c, float *humidity_percent)
{
    if (!s || !temperature_c || !humidity_percent) return ESP_ERR_INVALID_ARG;
    const uint8_t trigger[] = {0xAC, 0x33, 0x00};
    esp_err_t err = i2c_master_transmit(s->dev, trigger, sizeof(trigger), 100);
    if (err != ESP_OK) return err;
    delay_ms(80);
    uint8_t data[7];
    for (int attempt = 0; attempt < 20; ++attempt) {
        err = i2c_master_receive(s->dev, data, sizeof(data), 100);
        if (err != ESP_OK) return err;
        if (!(data[0] & 0x80)) {
            if (!(data[0] & 0x08)) return ESP_ERR_INVALID_RESPONSE;
            if (crc8(data, 6) != data[6]) return ESP_ERR_INVALID_CRC;
            uint32_t raw_h = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
            uint32_t raw_t = ((uint32_t)(data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];
            *humidity_percent = raw_h * (100.0f / 1048576.0f);
            *temperature_c = raw_t * (200.0f / 1048576.0f) - 50.0f;
            return ESP_OK;
        }
        delay_ms(10);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t aht20_deinit(aht20_handle_t s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    esp_err_t err = i2c_master_bus_rm_device(s->dev);
    if (err == ESP_OK) free(s);
    return err;
}
