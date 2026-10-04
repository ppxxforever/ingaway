idf_component_register(SRCS "aht20.c"
                       INCLUDE_DIRS "include"
                       REQUIRES I2c_bus
                       PRIV_REQUIRES freertos)
