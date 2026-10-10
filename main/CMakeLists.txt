idf_component_register(
    SRCS "main.c" "backlight.c" "battery.c" "dns_server.c" "feed.c" "improv.c" "net.c" "openaip.c" "ota.c" "radar_ui.c"
         "route.c" "settings.c" "trails.c" "web.c"
    INCLUDE_DIRS "."
    # "board" is boards/<OVERHEAD_BOARD>/board, added by the top-level CMakeLists.txt
    REQUIRES app_update board esp_app_format esp_https_ota esp_http_client esp_http_server esp_netif esp_wifi esp_timer json
             lwip mbedtls nvs_flash
    EMBED_TXTFILES "web/setup.html" "web/config.html"
)
