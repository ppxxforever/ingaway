idf_component_register(SRCS "ingaway.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_event wifi interaction w25q64)
