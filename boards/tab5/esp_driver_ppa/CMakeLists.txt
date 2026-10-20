# A patched copy of ESP-IDF 6.1.0's PPA driver (README.md). It replaces the
# IDF's own, so on any other IDF it would swap that IDF's driver for this old
# one: stop, so the copy is redone from the new IDF, or dropped if it fixes
# the hang.
if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" STREQUAL "6.1.0")
    message(FATAL_ERROR "boards/tab5/esp_driver_ppa is a patched copy of ESP-IDF 6.1.0's driver, but this is "
                        "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}: see its README.md")
endif()

idf_build_get_property(target IDF_TARGET)

set(srcs)
set(public_include "include")
if(CONFIG_SOC_PPA_SUPPORTED)
    list(APPEND srcs "src/ppa_core.c"
                     "src/ppa_srm.c"
                     "src/ppa_blend.c"
                     "src/ppa_fill.c")
    if(CONFIG_SOC_PAU_SUPPORTED)
        list(APPEND srcs "${target}/ppa_retention.c")
    endif()
endif()

if(${target} STREQUAL "linux")
    set(priv_requires "")
else()
    set(priv_requires esp_mm esp_pm esp_driver_dma efuse)
endif()


idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${public_include}
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES esp_hal_ppa
                       PRIV_REQUIRES "${priv_requires}"
                      )
