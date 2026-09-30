set(requires esp_http_client esp_netif esp_wifi esp_timer json mbedtls nvs_flash)
# The P4 BSP is a managed dependency (idf_component.yml) and is linked
# automatically; the S3 one is vendored under boards/ and must be named.
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND requires waveshare__esp32_s3_touch_lcd_28c)
endif()

idf_component_register(
    SRCS "main.c" "feed.c" "net.c" "radar_ui.c"
    INCLUDE_DIRS "."
    REQUIRES ${requires}
)
