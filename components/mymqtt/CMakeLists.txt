idf_component_register(SRCS "mymqtt.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES mqtt bmp280 aht20 environment
                    EMBED_TXTFILES
                        "certs/ca.crt"
                        "certs/client.crt"
                        "certs/client.key"
                    )
