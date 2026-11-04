# =============================================================================
# HiveShock for Dusklight
#
#   cmake -B build -S .
#   cmake --build build --config Release --target hiveshock_package
#
# Output: build/mods/hiveshock.dusk
#
# The SDK is fetched automatically (see cmake/FetchDusklight.cmake). To build against a
# local Dusklight checkout instead:  cmake -B build -S . -DDUSKLIGHT_DIR=D:/path/to/dusklight
# =============================================================================
cmake_minimum_required(VERSION 3.25)
project(hiveshock CXX)

# Dusklight commit this mod is built and tested against (v2.0.0).
# The mod reads game structs directly, so keep this in step with the Dusklight you run.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "e9b120544cb75e81b5aa36777f1688fa61f2d9e8")
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

add_mod(hiveshock
    FEATURES game fmt
    SOURCES
        src/mod.cpp
        src/net_bridge.cpp
        src/effects.cpp
    MOD_JSON mod.json
    RES_DIR res
)

# Platform-specific link libraries for the TCP networking module.
if (WIN32)
    target_link_libraries(hiveshock PRIVATE Ws2_32 Iphlpapi)
endif ()
