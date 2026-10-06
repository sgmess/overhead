idf_component_register(
    SRCS "main.c" "battery.c" "feed.c" "net.c" "radar_ui.c"
    INCLUDE_DIRS "."
    # "board" is boards/<OVERHEAD_BOARD>/board, added by the top-level CMakeLists.txt
    REQUIRES board esp_http_client esp_netif esp_wifi esp_timer json mbedtls nvs_flash
)
