idf_component_register(SRCS "blufi.c" "blufi_security.c"
                    INCLUDE_DIRS "include"
                    REQUIRES bt nvs_flash mbedtls
                    )
