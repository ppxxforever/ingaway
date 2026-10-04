#include "environment.h"
#include "i2c_bus.h"
#include "bmp280.h"
#include "aht20.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "env";
static TaskHandle_t task_handle;
static i2c_master_bus_handle_t bus;

bmp280_handle_t bmp = NULL;
aht20_handle_t aht = NULL;

static void environment_task(void *arg)
{
    (void)arg;
    while (true) {
        /* Missing sensors are retried independently, without blocking Wi-Fi. */
        if (!bmp) {
            esp_err_t err = bmp280_init(bus, CONFIG_ENV_BMP280_ADDRESS, &bmp);
            if (err == ESP_OK) ESP_LOGW(TAG, "BMP280 init succeeded");
            else ESP_LOGW(TAG, "BMP280 init failed: %s", esp_err_to_name(err));
        }
        if (!aht) {
            esp_err_t err = aht20_init(bus, &aht);
            if (err == ESP_OK) ESP_LOGW(TAG, "AHT20 init succeeded");
            else ESP_LOGW(TAG, "AHT20 init failed: %s", esp_err_to_name(err));
        }
        if(bmp && aht){
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ENV_SAMPLE_INTERVAL_MS));
    }
}

esp_err_t environment_start(void)
{
    if (task_handle || bus) return ESP_ERR_INVALID_STATE;
    esp_err_t err = i2c_bus_init(I2C_NUM_0, CONFIG_ENV_SDA_GPIO, CONFIG_ENV_SCL_GPIO, &bus);
    if (err != ESP_OK) return err;
    if (xTaskCreate(environment_task, "environment", 4096, NULL, 5, &task_handle) != pdPASS) {
        if (i2c_bus_deinit(bus) == ESP_OK) bus = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "I2C0 SDA=%d SCL=%d, BMP280=0x%02x, AHT20=0x38",
             CONFIG_ENV_SDA_GPIO, CONFIG_ENV_SCL_GPIO, CONFIG_ENV_BMP280_ADDRESS);
    return ESP_OK;
}
