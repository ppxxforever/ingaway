idf_component_register(SRCS "w25q64.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_partition
                       PRIV_REQUIRES esp_driver_spi spi_flash log)
