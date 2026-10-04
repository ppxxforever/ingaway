#include "bmp280.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct bmp280 {
    i2c_master_dev_handle_t dev;
    uint16_t t1, p1;
    int16_t t2, t3, p2, p3, p4, p5, p6, p7, p8, p9;
};

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static esp_err_t read_reg(bmp280_handle_t s, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s->dev, &reg, 1, data, len, 100);
}

static esp_err_t write_reg(bmp280_handle_t s, uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(s->dev, data, sizeof(data), 100);
}

esp_err_t bmp280_init(i2c_master_bus_handle_t bus, uint8_t address,
                      bmp280_handle_t *out_sensor)
{
    if (!bus || !out_sensor || (address != 0x76 && address != 0x77)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_sensor = NULL;
    bmp280_handle_t s = calloc(1, sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &config, &s->dev);
    if (err != ESP_OK) { free(s); return err; }
    uint8_t id, status = 1, c[24];
    err = read_reg(s, 0xD0, &id, 1);
    if (err != ESP_OK) goto fail;
    if (id != 0x58) { err = ESP_ERR_NOT_SUPPORTED; goto fail; }
    err = write_reg(s, 0xE0, 0xB6); /* Soft reset. */
    if (err != ESP_OK) goto fail;
    delay_ms(10);
    for (int i = 0; i < 20; ++i) {
        err = read_reg(s, 0xF3, &status, 1);
        if (err != ESP_OK) goto fail;
        if (!(status & 1)) break;
        delay_ms(5);
    }
    if (status & 1) { err = ESP_ERR_TIMEOUT; goto fail; }
    err = read_reg(s, 0x88, c, sizeof(c));
    if (err != ESP_OK) goto fail;
    s->t1 = le16(c); s->t2 = (int16_t)le16(c + 2); s->t3 = (int16_t)le16(c + 4);
    s->p1 = le16(c + 6); s->p2 = (int16_t)le16(c + 8); s->p3 = (int16_t)le16(c + 10);
    s->p4 = (int16_t)le16(c + 12); s->p5 = (int16_t)le16(c + 14); s->p6 = (int16_t)le16(c + 16);
    s->p7 = (int16_t)le16(c + 18); s->p8 = (int16_t)le16(c + 20); s->p9 = (int16_t)le16(c + 22);
    if (!s->p1) { err = ESP_ERR_INVALID_RESPONSE; goto fail; }
    *out_sensor = s;
    return ESP_OK;
fail:
    i2c_master_bus_rm_device(s->dev);
    free(s);
    return err;
}

esp_err_t bmp280_read(bmp280_handle_t s, float *temperature_c, float *pressure_hpa)
{
    if (!s || !temperature_c || !pressure_hpa) return ESP_ERR_INVALID_ARG;
    /* Temperature x1, pressure x1, forced mode. */
    esp_err_t err = write_reg(s, 0xF4, 0x25);
    if (err != ESP_OK) return err;
    delay_ms(10);
    uint8_t status = 0;
    for (int i = 0; i < 20; ++i) {
        err = read_reg(s, 0xF3, &status, 1);
        if (err != ESP_OK) return err;
        if (!(status & 0x09)) break;
        delay_ms(5);
    }
    if (status & 0x09) return ESP_ERR_TIMEOUT;
    uint8_t data[6];
    err = read_reg(s, 0xF7, data, sizeof(data));
    if (err != ESP_OK) return err;
    int32_t raw_p = ((uint32_t)data[0] << 12) | ((uint32_t)data[1] << 4) | (data[2] >> 4);
    int32_t raw_t = ((uint32_t)data[3] << 12) | ((uint32_t)data[4] << 4) | (data[5] >> 4);
    if (raw_p == 0x80000 || raw_t == 0x80000) return ESP_ERR_INVALID_RESPONSE;
    /* Bosch datasheet double-precision compensation algorithm. */
    double v1 = (raw_t / 16384.0 - s->t1 / 1024.0) * s->t2;
    double v2 = (raw_t / 131072.0 - s->t1 / 8192.0);
    v2 = v2 * v2 * s->t3;
    double fine = v1 + v2;
    double temperature = fine / 5120.0;
    v1 = fine / 2.0 - 64000.0;
    v2 = v1 * v1 * s->p6 / 32768.0;
    v2 += v1 * s->p5 * 2.0;
    v2 = v2 / 4.0 + s->p4 * 65536.0;
    v1 = (s->p3 * v1 * v1 / 524288.0 + s->p2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * s->p1;
    if (v1 == 0.0) return ESP_ERR_INVALID_RESPONSE;
    double pressure = (1048576.0 - raw_p - v2 / 4096.0) * 6250.0 / v1;
    v1 = s->p9 * pressure * pressure / 2147483648.0;
    v2 = pressure * s->p8 / 32768.0;
    pressure += (v1 + v2 + s->p7) / 16.0;
    *temperature_c = (float)temperature;
    *pressure_hpa = (float)(pressure / 100.0);
    return ESP_OK;
}

esp_err_t bmp280_deinit(bmp280_handle_t s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    esp_err_t err = i2c_master_bus_rm_device(s->dev);
    if (err == ESP_OK) free(s);
    return err;
}
