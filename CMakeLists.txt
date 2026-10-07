cmake_minimum_required(VERSION 3.16)

# One app, several boards, chosen when a build directory is first configured:
#
#   idf.py build                                         # P4 3.4C (default), in ./build
#   idf.py -B build_s3 -D OVERHEAD_BOARD=s3_28c build     # S3 2.8C
#   idf.py -B build_tab5 -D OVERHEAD_BOARD=tab5 build     # M5Stack Tab5
#
# A CMake variable rather than Kconfig, because it has to be known before Kconfig
# runs: it picks the chip, the board component and the defaults files. Each board
# keeps its own sdkconfig and dependencies.lock so building one never touches
# the other's settings.
set(OVERHEAD_BOARDS p4_34c s3_28c tab5)
set(OVERHEAD_BOARD "p4_34c" CACHE STRING "Target board: ${OVERHEAD_BOARDS}")
set_property(CACHE OVERHEAD_BOARD PROPERTY STRINGS ${OVERHEAD_BOARDS})

if(NOT OVERHEAD_BOARD IN_LIST OVERHEAD_BOARDS)
    message(FATAL_ERROR "OVERHEAD_BOARD='${OVERHEAD_BOARD}' is not one of: ${OVERHEAD_BOARDS}")
endif()
if(DEFINED CACHE{OVERHEAD_BOARD_CONFIGURED} AND NOT OVERHEAD_BOARD STREQUAL OVERHEAD_BOARD_CONFIGURED)
    set(requested "${OVERHEAD_BOARD}")
    set(OVERHEAD_BOARD "${OVERHEAD_BOARD_CONFIGURED}" CACHE STRING "Target board: ${OVERHEAD_BOARDS}" FORCE)
    message(FATAL_ERROR "${CMAKE_BINARY_DIR} is configured for ${OVERHEAD_BOARD_CONFIGURED}, not "
                        "${requested}. Use a separate build directory per board (idf.py -B build_s3 ...).")
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
# over-the-air update for another board is refused (see main/ota.c)
project(overhead-${OVERHEAD_BOARD})
