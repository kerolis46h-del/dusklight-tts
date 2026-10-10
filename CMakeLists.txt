cmake_minimum_required(VERSION 3.26)
project(tts_mod CXX)

if (NOT DUSKLIGHT_VERSION)
    # Must match the Dusklight version you play (tag v2.0.3).
    set(DUSKLIGHT_VERSION "40457c6adb381928e4b5fef6ed459ed291edd5e2")
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

add_mod(tts_mod
        FEATURES game fmt
        SOURCES src/mod.cpp
        MOD_JSON mod.json
)

if (WIN32 AND MSVC)
    target_link_libraries(tts_mod PRIVATE ole32 sapi)
endif ()

