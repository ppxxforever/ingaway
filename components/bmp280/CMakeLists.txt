idf_component_register(SRCS "bmp280.c"
                       INCLUDE_DIRS "include"
                       REQUIRES I2c_bus
                       PRIV_REQUIRES freertos)
