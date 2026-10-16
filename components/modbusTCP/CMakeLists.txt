idf_component_register(SRCS "modbusTCP.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp-modbus esp_netif
                    PRIV_REQUIRES esp_timer esp_system heap w25q64
                    )
