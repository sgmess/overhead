cmake_minimum_required(VERSION 3.16)

# One app, several boards, chosen when a build directory is first configured:
#
#   idf.py build                                         # P4 3.4C (default), in ./build
#   idf.py -B build_tab5 -D OVERHEAD_BOARD=tab5 build     # M5Stack Tab5
#
# A CMake variable rather than Kconfig, because it has to be known before Kconfig
# runs: it picks the chip, the board component and the defaults files. Each board
# keeps its own sdkconfig and dependencies.lock, so building one never changes
# the other's settings. managed_components/ is shared, though: switching boards
# re-fetches the components the new one needs.
set(OVERHEAD_BOARDS p4_34c tab5)
set(OVERHEAD_BOARD "p4_34c" CACHE STRING "Target board: ${OVERHEAD_BOARDS}")
set_property(CACHE OVERHEAD_BOARD PROPERTY STRINGS ${OVERHEAD_BOARDS})

if(NOT OVERHEAD_BOARD IN_LIST OVERHEAD_BOARDS)
    message(FATAL_ERROR "OVERHEAD_BOARD='${OVERHEAD_BOARD}' is not one of: ${OVERHEAD_BOARDS}")
endif()
if(DEFINED CACHE{OVERHEAD_BOARD_CONFIGURED} AND NOT OVERHEAD_BOARD STREQUAL OVERHEAD_BOARD_CONFIGURED)
    set(requested "${OVERHEAD_BOARD}")
    set(OVERHEAD_BOARD "${OVERHEAD_BOARD_CONFIGURED}" CACHE STRING "Target board: ${OVERHEAD_BOARDS}" FORCE)
    message(FATAL_ERROR "${CMAKE_BINARY_DIR} is configured for ${OVERHEAD_BOARD_CONFIGURED}, not "
                        "${requested}. Use a separate build directory per board (idf.py -B build_tab5 ...).")
endif()
set(OVERHEAD_BOARD_CONFIGURED "${OVERHEAD_BOARD}" CACHE INTERNAL "Board this build directory belongs to")

# boards/<board>/ holds that board's "board" component (bring-up code, plus the
# BSP and WiFi bridge as dependencies) and any vendored BSP. Only the selected
# board's directory is in the build.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards/${OVERHEAD_BOARD}")

set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.${OVERHEAD_BOARD}")
set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${OVERHEAD_BOARD}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${OVERHEAD_BOARD}")

# The board is in the name, so the app image says which board it is for and an
# over-the-air update for another board is refused (see main/net/ota.c)
project(overhead-${OVERHEAD_BOARD})

# esp_lvgl_adapter ~0.6 (the 3.4C's BSP pins it; the Tab5 uses it too)
# includes LVGL 9.6's deprecated src/ headers. Their #warning is an error
# under IDF 6.1's -Werror, so in that component alone it stays a warning.
idf_component_get_property(lvgl_adapter_lib espressif__esp_lvgl_adapter COMPONENT_LIB)
target_compile_options(${lvgl_adapter_lib} PRIVATE -Wno-error=cpp)
