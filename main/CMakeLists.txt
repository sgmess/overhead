idf_component_register(
    SRCS "main.c" "battery.c" "dns_server.c" "feed.c" "net.c" "openaip.c" "radar_ui.c" "settings.c" "web.c"
    INCLUDE_DIRS "."
    # "board" is boards/<OVERHEAD_BOARD>/board, added by the top-level CMakeLists.txt
    REQUIRES board esp_app_format esp_http_client esp_http_server esp_netif esp_wifi esp_timer json
             lwip mbedtls nvs_flash
    EMBED_TXTFILES "web/setup.html" "web/config.html"
)
