# By what each part does:
#   core/     the logic, without the network or the screen: counting, trails, the airline and country tables
#   sources/  what is fetched: aircraft, routes, owners and photos, airspace, the location
#   net/      WiFi, setup, the web server and its pages, updates
#   ui/       the screen: radar, card, big view, lists, settings, and the images they show
#   device/   settings storage and the battery
idf_component_register(
    SRCS "main.c"
         "core/airlines.c" "core/countries.c" "core/stats.c" "core/trails.c"
         "sources/details.c" "sources/feed.c" "sources/geoloc.c" "sources/openaip.c" "sources/route.c" "sources/zones.c"
         "net/dns_server.c" "net/improv.c" "net/net.c" "net/ota.c" "net/web.c"
         "ui/backlight.c" "ui/flags.c" "ui/logos.c" "ui/lvgl_mem.c" "ui/radar_ui.c" "ui/scope.c" "ui/card.c" "ui/big_view.c" "ui/lists.c" "ui/describe.c" "ui/settings_ui.c" "ui/splash.c"
         "device/battery.c" "device/settings.c"
         "third_party/stb_image.c"
    # Headers by name alone, wherever they are; "." for third_party/
    INCLUDE_DIRS "." "core" "sources" "net" "ui" "device"
    # "board" is boards/<OVERHEAD_BOARD>/board, added by the top-level CMakeLists.txt
    REQUIRES app_update board esp_app_format esp_https_ota esp_http_client esp_http_server esp_netif esp_wifi esp_timer
             esp_partition esp_rom lwip mbedtls nvs_flash
    # The symbols are named for the file alone: _binary_config_html_start
    EMBED_TXTFILES "net/web/setup.html" "net/web/config.html"
    EMBED_FILES "ui/flags.bin" "ui/logos.bin"
)

# Someone else's code, kept as it comes
set_source_files_properties(third_party/stb_image.c PROPERTIES COMPILE_OPTIONS "-w")

# Nothing in this component calls the allocator in ui/lvgl_mem.c; LVGL does,
# from a library linked after this one. Undefined from the start, the
# linker takes lvgl_mem.o when it reaches this library.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
