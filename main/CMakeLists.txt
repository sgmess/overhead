idf_component_register(
    SRCS "main.c" "backlight.c" "battery.c" "lvgl_mem.c" "dns_server.c" "feed.c" "improv.c" "net.c" "openaip.c" "ota.c" "radar_ui.c"
         "route.c" "settings.c" "settings_ui.c" "splash.c" "trails.c" "web.c" "zones.c"
    INCLUDE_DIRS "."
    # "board" is boards/<OVERHEAD_BOARD>/board, added by the top-level CMakeLists.txt
    REQUIRES app_update board esp_app_format esp_https_ota esp_http_client esp_http_server esp_netif esp_wifi esp_timer
             esp_partition esp_rom lwip mbedtls nvs_flash
    EMBED_TXTFILES "web/setup.html" "web/config.html"
)

# Nothing in this component calls the allocator in lvgl_mem.c; LVGL does,
# from a library linked after this one. Undefined from the start, the
# linker takes lvgl_mem.o when it reaches this library.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
