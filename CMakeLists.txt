cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED ENV{CMAKE_BUILD_TYPE})
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type")
endif ()

project(tts_mod VERSION 1.0.0 LANGUAGES CXX)

# The Dusklight version to build against (v2.0.3).
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "40457c6adb381928e4b5fef6ed459ed291edd5e2")
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

add_mod(tts_mod
        FEATURES game fmt
        SOURCES src/mod.cpp
        MOD_JSON "${CMAKE_CURRENT_SOURCE_DIR}/mod.json"
)

if (MSVC)
    # Makes __cplusplus report the real C++ version (Dusklight headers rely on it).
    target_compile_options(tts_mod PRIVATE /Zc:__cplusplus)
    target_link_libraries(tts_mod PRIVATE ole32 sapi)
endif ()
