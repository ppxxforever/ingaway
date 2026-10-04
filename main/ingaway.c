#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "wifi.h"
#include "environment.h"
#include "w25q64.h"
#include "sdkconfig.h"

static const char *TAG = "main";

/**
 * Initialize NVS FLASH
 * @return ESP_OK on success, otherwise an error code
 */
int nvs_init(void)
{    
    int rc = 0;
    rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        rc = nvs_flash_init();
    }
    return 0;
}

void app_main(void)
{   

    // Initialize NVS FLASH
    ESP_LOGI(TAG, "Initializing NVS FLASH");
    int rc = 0;
    rc = nvs_init();
    ESP_ERROR_CHECK(rc);
    
    // 创建的“默认事件循环”
    ESP_LOGI(TAG, "Initializing default event loop");
    rc = esp_event_loop_create_default();
    ESP_ERROR_CHECK(rc);       

#ifdef CONFIG_W25Q64_ENABLED
    ESP_LOGI(TAG, "Initializing external storage");
    esp_err_t storage_err = w25q64_init();
    if (storage_err != ESP_OK) {
        ESP_LOGW(TAG, "External storage unavailable: %s", esp_err_to_name(storage_err));
    }
#endif

    ESP_LOGI(TAG, "Initializing wifi");
    wifi_init();
    
}
