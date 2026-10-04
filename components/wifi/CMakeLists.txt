idf_component_register(SRCS "wifi.c"
                    INCLUDE_DIRS "include"
                    REQUIRES  blufi nvs_flash esp_wifi mymqtt 
                    )
