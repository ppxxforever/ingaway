idf_component_register(SRCS "mymqtt.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES mqtt modbusTCP esp_timer esp_netif esp_hw_support w25q64
                    EMBED_TXTFILES
                        "certs/ca.crt"
                        "certs/client.crt"
                        "certs/client.key"
                    )
