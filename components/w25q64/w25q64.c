#include "w25q64.h"
#include "driver/spi_master.h"
#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "w25q64";
static esp_flash_t *chip;
static bool bus_initialized;
static const esp_partition_t *history, *cache, *logs;
static bool ready;

const esp_partition_t *w25q64_history_partition(void) { return ready ? history : NULL; }
const esp_partition_t *w25q64_cache_partition(void) { return ready ? cache : NULL; }
const esp_partition_t *w25q64_logs_partition(void) { return ready ? logs : NULL; }

static esp_err_t cleanup(void)
{
    const esp_partition_t **parts[] = {&logs, &cache, &history};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        if (*parts[i]) {
            esp_err_t err = esp_partition_deregister_external(*parts[i]);
            if (err != ESP_OK) return err;
            *parts[i] = NULL;
        }
    }
    if (chip) {
        esp_err_t err = spi_bus_remove_flash_device(chip);
        if (err != ESP_OK) return err;
        chip = NULL;
    }
    if (bus_initialized) {
        esp_err_t err = spi_bus_free(SPI2_HOST);
        if (err != ESP_OK) return err;
        bus_initialized = false;
    }
    return ESP_OK;
}

esp_err_t w25q64_init(void)
{
    if (ready || chip || bus_initialized) return ESP_ERR_INVALID_STATE;
    const int pins[] = {CONFIG_W25Q64_CS_GPIO, CONFIG_W25Q64_MOSI_GPIO,
                        CONFIG_W25Q64_SCLK_GPIO, CONFIG_W25Q64_MISO_GPIO};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        for (size_t j = i + 1; j < sizeof(pins) / sizeof(pins[0]); ++j) {
            if (pins[i] == pins[j]) return ESP_ERR_INVALID_ARG;
        }
    }
    const spi_bus_config_t bus_config = {
        .mosi_io_num = CONFIG_W25Q64_MOSI_GPIO,
        .miso_io_num = CONFIG_W25Q64_MISO_GPIO,
        .sclk_io_num = CONFIG_W25Q64_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    bus_initialized = true;
    const esp_flash_spi_device_config_t flash_config = {
        .host_id = SPI2_HOST,
        .cs_io_num = CONFIG_W25Q64_CS_GPIO,
        .io_mode = SPI_FLASH_SLOWRD,
        .freq_mhz = 10,
        .clock_source = SPI_CLK_SRC_DEFAULT,
    };
    err = spi_bus_add_flash_device(&chip, &flash_config);
    if (err != ESP_OK) goto fail;
    err = esp_flash_init(chip);
    if (err != ESP_OK) goto fail;
    uint32_t id, size;
    err = esp_flash_read_id(chip, &id);
    if (err != ESP_OK) goto fail;
    err = esp_flash_get_size(chip, &size);
    if (err != ESP_OK) goto fail;
    /* Winbond JEDEC manufacturer 0xEF, capacity code 0x17 = 64 Mbit.
       Memory-type byte varies across W25Q64 variants. */
    if (((id >> 16) & 0xFF) != 0xEF || (id & 0xFF) != 0x17 || size != 0x800000) {
        ESP_LOGE(TAG, "Expected W25Q64 8 MiB, detected ID=0x%06lx size=%lu",
                 (unsigned long)id, (unsigned long)size);
        err = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }
    /* Runtime external partitions are NOT entries in internal partitions.csv. */
    err = esp_partition_register_external(chip, 0, 0x600000, "ext_history",
            ESP_PARTITION_TYPE_DATA, 0x41, &history);
    if (err != ESP_OK) goto fail;
    err = esp_partition_register_external(chip, 0x600000, 0x100000, "ext_cache",
            ESP_PARTITION_TYPE_DATA, 0x40, &cache);
    if (err != ESP_OK) goto fail;
    err = esp_partition_register_external(chip, 0x700000, 0x100000, "ext_logs",
            ESP_PARTITION_TYPE_DATA, 0x42, &logs);
    if (err != ESP_OK) goto fail;
    ready = true;
    ESP_LOGI(TAG, "ID=0x%06lx, 8 MiB: history 6 MiB, cache 1 MiB, logs 1 MiB",
             (unsigned long)id);
    return ESP_OK;
fail:
    {
        esp_err_t cleanup_err = cleanup();
        if (cleanup_err != ESP_OK) {
            ESP_LOGE(TAG, "Cleanup failed: %s", esp_err_to_name(cleanup_err));
        }
    }
    return err;
}
