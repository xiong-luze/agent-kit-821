cmake_minimum_required(VERSION 3.20)
project(v821_aec_probe C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# This project deliberately has no cloud SDK, LVGL, or credential dependency.
set(SPEEX_ROOT "" CACHE PATH "SpeexDSP installation for the selected target ABI")
if(SPEEX_ROOT)
    # An explicit target installation may live outside the compiler sysroot.
    # Do not silently fall back to host libraries if that installation is missing.
    find_path(SPEEX_INCLUDE_DIR speex/speex_echo.h PATHS "${SPEEX_ROOT}/include"
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(SPEEX_LIBRARY NAMES speexdsp PATHS "${SPEEX_ROOT}/lib"
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
else()
    find_path(SPEEX_INCLUDE_DIR speex/speex_echo.h)
    find_library(SPEEX_LIBRARY NAMES speexdsp)
endif()
if(NOT SPEEX_INCLUDE_DIR OR NOT SPEEX_LIBRARY)
    message(FATAL_ERROR "SpeexDSP not found; set SPEEX_ROOT to its target installation")
endif()

add_executable(v821_aec_probe aec_probe.c)
target_include_directories(v821_aec_probe PRIVATE "${SPEEX_INCLUDE_DIR}")
target_link_libraries(v821_aec_probe PRIVATE "${SPEEX_LIBRARY}" m)
target_compile_definitions(v821_aec_probe PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(v821_aec_probe PRIVATE -Wall -Wextra -Werror)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    option(AEC_WITH_ALSA "Build continuous capture/playback probe" ON)
else()
    option(AEC_WITH_ALSA "Build continuous capture/playback probe" OFF)
endif()
if(AEC_WITH_ALSA)
    find_package(ALSA REQUIRED)
    target_sources(v821_aec_probe PRIVATE pcm_alsa.c)
    target_include_directories(v821_aec_probe PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_libraries(v821_aec_probe PRIVATE ${ALSA_LIBRARIES})
    target_compile_definitions(v821_aec_probe PRIVATE HAVE_ALSA=1)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME aec_replay
        COMMAND "${CMAKE_COMMAND}" -E env "V821_AEC_PROBE=$<TARGET_FILE:v821_aec_probe>"
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_probe.py")
endif()
