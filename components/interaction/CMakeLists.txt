idf_component_register(SRCS "lcd.c" "interaction.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_timer wifi)
