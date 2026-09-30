idf_component_register(
    SRCS "esp32_s3_touch_lcd_28c.c"
    INCLUDE_DIRS "include" "include/bsp"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_spi esp_lcd
    PRIV_REQUIRES esp_timer esp_psram
)
