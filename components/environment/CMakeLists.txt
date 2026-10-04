idf_component_register(SRCS "environment.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES I2c_bus bmp280 aht20 freertos log)
