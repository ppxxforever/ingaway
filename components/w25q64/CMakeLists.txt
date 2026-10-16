idf_component_register(SRCS "w25q64_cache.c" "flash_fifo.c" "w25q64.c" "storage_archive.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_partition
                       PRIV_REQUIRES esp_driver_spi spi_flash log esp_timer esp_hw_support nvs_flash freertos)
