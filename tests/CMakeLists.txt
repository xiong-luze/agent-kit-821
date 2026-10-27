find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(AK "${CMAKE_CURRENT_SOURCE_DIR}/../../agentic-kit" CACHE PATH "SDK headers for host compilation checks")
set(VOICE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(SPEEX_ROOT "${VOICE_ROOT}/build/aec-deps/speexdsp-install" CACHE PATH "Host SpeexDSP installation")
set(POCKETSPHINX_ROOT "" CACHE PATH "Host PocketSphinx 5.0.4 installation (optional)")
set(WAKE_TEST_PREPARED "" CACHE PATH "Prepared external Mandarin model/dictionary for real worker test")
set(WAKE_ACOUSTIC_FIXTURES "" CACHE FILEPATH "Optional strict real-PCM acoustic quality manifest")
set(LVGL_HOST_ROOT "" CACHE PATH "Optional LVGL8.3 headers for UI translation-unit check")
set(VOICE_FAR_PCM "" CACHE FILEPATH "External far-end raw PCM for actual DSP regression")
set(VOICE_NEAR_PCM "" CACHE FILEPATH "External near-end raw PCM for actual DSP regression")
option(V821_TEST_UBSAN "Instrument host voice tests with UBSan" OFF)

add_compile_options(-Wall -Wextra -Werror)
add_compile_definitions(_POSIX_C_SOURCE=200809L)
if(V821_TEST_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all)
    add_link_options(-fsanitize=undefined)
endif()
include_directories("${VOICE_ROOT}" "${VOICE_ROOT}/core" "${VOICE_ROOT}/service" "${VOICE_ROOT}/bsp/include"
    "${AK}/modules/rtc-tcp-client/include" "${AK}/modules/iot-client/include"
    "${AK}/modules/atop/include" "${AK}/pal" "${AK}/common"
    "${AK}/third_party/mbedtls/include")

add_executable(test_dialog test_dialog.c ../core/dialog.c ../core/pcm_history.c)
add_test(NAME dialog COMMAND test_dialog)
add_executable(test_config test_config.c ../core/app_config.c ../core/ringbuf.c)
target_link_libraries(test_config PRIVATE Threads::Threads m)
add_test(NAME config_and_ring COMMAND test_config)
add_executable(test_app_duplex test_app_duplex.c ../core/app_duplex.c
    ../core/app_config.c ../core/dialog.c ../core/echo_guard.c ../core/pcm_history.c ../core/ringbuf.c)
target_link_libraries(test_app_duplex PRIVATE Threads::Threads m)
add_test(NAME actual_duplex_loop COMMAND test_app_duplex)
add_executable(test_app_half_duplex test_app_half_duplex.c ../core/ringbuf.c)
target_link_libraries(test_app_half_duplex PRIVATE Threads::Threads)
add_test(NAME actual_half_duplex_alsa_clock COMMAND test_app_half_duplex)

add_executable(test_session_primitives session/test_session_primitives.c
    ../service/audio_uplink.c ../service/turn_guard.c)
target_link_libraries(test_session_primitives PRIVATE Threads::Threads)
add_test(NAME session_queue_and_guard COMMAND test_session_primitives)
add_executable(test_session_callbacks session/test_session_callbacks.c
    ../service/audio_uplink.c ../service/turn_guard.c ../core/ringbuf.c
    ../core/jsonx.c ../core/text.c)
target_link_libraries(test_session_callbacks PRIVATE Threads::Threads)
add_test(NAME actual_session_callbacks COMMAND test_session_callbacks)
add_executable(test_mcp_generation session/test_mcp_generation.c ../core/jsonx.c)
target_link_libraries(test_mcp_generation PRIVATE Threads::Threads)
add_test(NAME actual_mcp_generation COMMAND test_mcp_generation)

add_executable(test_wake_unavailable wake/test_worker.c ../core/wake_word.c)
target_link_libraries(test_wake_unavailable PRIVATE Threads::Threads)
add_test(NAME wake_unavailable_fails_closed COMMAND test_wake_unavailable)
add_test(NAME wake_model_preparer COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/wake/test_prepare.py")
add_test(NAME wake_fixture_runner COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/wake/test_fixture_runner.py")
if(POCKETSPHINX_ROOT)
    find_path(PS_INCLUDE pocketsphinx.h PATHS "${POCKETSPHINX_ROOT}/include" NO_DEFAULT_PATH)
    find_library(PS_LIBRARY pocketsphinx PATHS "${POCKETSPHINX_ROOT}/lib" NO_DEFAULT_PATH)
    if(NOT PS_INCLUDE OR NOT PS_LIBRARY)
        message(FATAL_ERROR "POCKETSPHINX_ROOT does not contain a host installation")
    endif()
    add_executable(test_wake_worker wake/test_worker.c ../core/wake_word.c)
    target_compile_definitions(test_wake_worker PRIVATE HAVE_POCKETSPHINX=1)
    target_include_directories(test_wake_worker SYSTEM PRIVATE "${PS_INCLUDE}")
    target_link_libraries(test_wake_worker PRIVATE "${PS_LIBRARY}" Threads::Threads m)
    add_executable(wake_probe ../tools/wake/wake_probe.c ../core/wake_word.c)
    target_compile_definitions(wake_probe PRIVATE HAVE_POCKETSPHINX=1)
    target_include_directories(wake_probe SYSTEM PRIVATE "${PS_INCLUDE}")
    target_link_libraries(wake_probe PRIVATE "${PS_LIBRARY}" Threads::Threads m)
    if(WAKE_TEST_PREPARED AND VOICE_FAR_PCM)
        add_library(wake_joined_subject OBJECT ../core/wake_word.c)
        target_compile_definitions(wake_joined_subject PRIVATE HAVE_POCKETSPHINX=1 ps_get_hyp=joined_hypothesis)
        target_include_directories(wake_joined_subject SYSTEM PRIVATE "${PS_INCLUDE}")
        add_executable(test_wake_joined wake/test_joined_result.c $<TARGET_OBJECTS:wake_joined_subject>)
        target_include_directories(test_wake_joined SYSTEM PRIVATE "${PS_INCLUDE}")
        target_link_libraries(test_wake_joined PRIVATE "${PS_LIBRARY}" Threads::Threads m)
        add_test(NAME joined_keyword_result COMMAND test_wake_joined "${WAKE_TEST_PREPARED}"
            "${WAKE_TEST_PREPARED}/wake.dict" "${VOICE_FAR_PCM}")
    endif()
    if(WAKE_TEST_PREPARED)
        add_library(wake_quiet_subject OBJECT ../core/wake_word.c)
        target_compile_definitions(wake_quiet_subject PRIVATE HAVE_POCKETSPHINX=1 ps_process_raw=observe_quiet_channel)
        target_include_directories(wake_quiet_subject SYSTEM PRIVATE "${PS_INCLUDE}")
        add_executable(test_wake_quiet_channel wake/test_quiet_channel.c $<TARGET_OBJECTS:wake_quiet_subject>)
        target_include_directories(test_wake_quiet_channel SYSTEM PRIVATE "${PS_INCLUDE}")
        target_link_libraries(test_wake_quiet_channel PRIVATE "${PS_LIBRARY}" Threads::Threads m)
        add_test(NAME quiet_keyword_channel COMMAND test_wake_quiet_channel "${WAKE_TEST_PREPARED}"
            "${WAKE_TEST_PREPARED}/wake.dict")
        add_test(NAME real_wake_worker COMMAND test_wake_worker
            "${WAKE_TEST_PREPARED}" "${WAKE_TEST_PREPARED}/wake.dict")
    endif()
    if(WAKE_ACOUSTIC_FIXTURES)
        add_test(NAME wake_acoustic_quality COMMAND "${Python3_EXECUTABLE}"
            "${VOICE_ROOT}/tools/wake/check_fixtures.py" --probe "$<TARGET_FILE:wake_probe>"
            --manifest "${WAKE_ACOUSTIC_FIXTURES}")
    endif()
elseif(WAKE_ACOUSTIC_FIXTURES OR WAKE_TEST_PREPARED)
    message(FATAL_ERROR "Real wake checks require POCKETSPHINX_ROOT")
endif()

# Actual application translation units, with real SDK/DSP headers. No firmware
# is linked here; UI/camera/SDK target binaries and real credentials are not used.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/tuya_authkey.h"
    "/* HOST COMPILE CHECK ONLY: these are not device credentials. */\n"
    "#define TUYA_UUID \"HOST_COMPILE_ONLY\"\n"
    "#define TUYA_AUTH_KEY \"HOST_COMPILE_ONLY\"\n"
    "#define TUYA_PRODUCT_KEY \"HOST_COMPILE_ONLY\"\n")
add_library(app_compile_check OBJECT ../app/main.c ../core/app.c ../core/app_config.c
    ../service/session.c ../service/mcp.c)
target_include_directories(app_compile_check PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_executable(test_main_reconnect test_main_reconnect.c ../core/app_config.c)
target_include_directories(test_main_reconnect PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME actual_product_reconnect COMMAND test_main_reconnect)
add_library(board_pipe_compile_check OBJECT ../bsp/boards/v821_aitoy/board.c)
target_include_directories(board_pipe_compile_check PRIVATE "${VOICE_ROOT}/bsp/boards/v821_aitoy")
add_library(board_pcm_compile_check OBJECT ../bsp/boards/v821_aitoy/board.c)
target_compile_definitions(board_pcm_compile_check PRIVATE HAVE_ALSA_PCM=1)
target_include_directories(board_pcm_compile_check PRIVATE "${VOICE_ROOT}/bsp/boards/v821_aitoy")
if(LVGL_HOST_ROOT)
    add_library(ui_compile_check OBJECT ../ui/ui.c)
    target_compile_definitions(ui_compile_check PRIVATE LV_CONF_INCLUDE_SIMPLE=1)
    target_include_directories(ui_compile_check PRIVATE "${VOICE_ROOT}/ui")
    target_include_directories(ui_compile_check SYSTEM PRIVATE "${LVGL_HOST_ROOT}"
        "${LVGL_HOST_ROOT}/src/extra/libs/qrcode")
endif()
find_path(DSP_INCLUDE speex/speex_echo.h PATHS "${SPEEX_ROOT}/include" NO_DEFAULT_PATH)
if(DSP_INCLUDE)
    add_library(dsp_compile_check OBJECT ../core/audio_dsp.c)
    target_include_directories(dsp_compile_check PRIVATE "${DSP_INCLUDE}")
    find_library(DSP_LIBRARY speexdsp PATHS "${SPEEX_ROOT}/lib" NO_DEFAULT_PATH)
    if(DSP_LIBRARY)
        add_executable(test_echo_guard echo_guard/test_guard.c ../core/echo_guard.c
            ../core/audio_dsp.c ../core/dialog.c)
        target_include_directories(test_echo_guard PRIVATE "${DSP_INCLUDE}")
        target_link_libraries(test_echo_guard PRIVATE "${DSP_LIBRARY}" m)
        add_executable(test_webrtc_dsp dsp/test_webrtc.c ../core/audio_dsp.c)
        target_compile_definitions(test_webrtc_dsp PRIVATE HAVE_WEBRTC_AEC=1)
        target_include_directories(test_webrtc_dsp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/dsp" "${DSP_INCLUDE}")
        target_link_libraries(test_webrtc_dsp PRIVATE "${DSP_LIBRARY}" m)
        add_test(NAME vendor_aec_contract COMMAND test_webrtc_dsp)
        add_test(NAME echo_guard_synthetic COMMAND test_echo_guard)
        if(VOICE_FAR_PCM AND VOICE_NEAR_PCM)
            add_test(NAME actual_dsp_and_barge_guard COMMAND test_echo_guard
                "${VOICE_FAR_PCM}" "${VOICE_NEAR_PCM}")
        endif()
    endif()
endif()

set(ALSA_TEST_INCLUDE "" CACHE PATH "Real ALSA headers for fake-device backend tests")
if(ALSA_TEST_INCLUDE)
    add_test(NAME alsa_control_boundary COMMAND "${CMAKE_COMMAND}" -E env
        "ALSA_TEST_INCLUDE=${ALSA_TEST_INCLUDE}" "CC=${CMAKE_C_COMPILER}"
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/audio_pcm/test_backend.py")
endif()
