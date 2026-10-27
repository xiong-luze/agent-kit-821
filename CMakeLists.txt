# V821 × agentic-kit —— 涂鸦 AI 语音方案在九鼎 V821 上的移植
#
# 分层见 docs/architecture.md：
#   bsp/     板级（V4L2 / fbdev / ALSA 管道 / mixer 配方），一板一目录
#   core/    平台无关（对话状态机 / VAD / AEC / ringbuf / JSON）
#   service/ 涂鸦云（会话 / 配网 / MCP）
#   ui/      界面（LVGL + 二维码 + 字体/emoji 资源）
#   app/     产品层（main）
#
# 构建：
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-v821.cmake
#   cmake --build build -j
# 第三方依赖需先按 README §2 准备到 third_party/
cmake_minimum_required(VERSION 3.20)
# 主体是 C99。带 CXX 只为链接 third_party/pdet 那几个 C++ 静态库
# （见文件末尾"本地人形检测"一节），我们自己不写 C++。
project(v821_chatbot C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(V821_BUILD_HOST_TESTS "Build voice logic tests without credentials/UI/cloud" OFF)
if(V821_BUILD_HOST_TESTS)
    enable_testing()
    add_subdirectory(tests)
    return()
endif()

# agentic-kit SDK（涂鸦上游）源码位置。默认按 README §2 的目录布局，
# 也可以 -DAK=<路径> 指定。
set(AK "$ENV{HOME}/kit/agentic-kit" CACHE PATH "agentic-kit SDK 源码位置")
if(NOT EXISTS "${AK}/CMakeLists.txt")
    message(FATAL_ERROR
        "找不到 agentic-kit：${AK}\n"
        "  git clone --recursive https://github.com/tuya/agentic-kit\n"
        "  然后 -DAK=<路径> 指定，或放到上面这个默认位置。")
endif()
set(AK_POSIX   "${AK}/examples/posix")
set(LVGL_DIR   "${CMAKE_CURRENT_LIST_DIR}/third_party/lvgl")
set(SPEEX_ROOT "${CMAKE_CURRENT_LIST_DIR}/third_party/speexdsp-install" CACHE PATH "Target SpeexDSP installation")
set(JPEG_ROOT  "${CMAKE_CURRENT_LIST_DIR}/third_party/jpeg-install")
set(PDET_ROOT  "${CMAKE_CURRENT_LIST_DIR}/third_party/pdet")
set(BOARD      "v821_aitoy" CACHE STRING "编进哪块板（bsp/boards/<BOARD>）")
set(V821_AUDIO_BACKEND "pipe" CACHE STRING "Audio backend: pipe or alsa")
set_property(CACHE V821_AUDIO_BACKEND PROPERTY STRINGS pipe alsa)
set(V821_AEC_BACKEND "speex" CACHE STRING "AEC algorithm: speex or SDK webrtc")
set_property(CACHE V821_AEC_BACKEND PROPERTY STRINGS speex webrtc)
set(WEBRTC_AEC_ROOT "" CACHE PATH "SDK aec_lib/webrtc directory with target musl libAec.a")
if(NOT V821_AEC_BACKEND STREQUAL "speex" AND NOT V821_AEC_BACKEND STREQUAL "webrtc")
    message(FATAL_ERROR "V821_AEC_BACKEND must be speex or webrtc")
endif()
if(V821_AEC_BACKEND STREQUAL "webrtc" AND NOT V821_AUDIO_BACKEND STREQUAL "alsa")
    message(FATAL_ERROR "SDK WebRTC requires the bounded, in-process ALSA backend")
endif()
option(V821_ENABLE_WAKE_WORD "Build local PocketSphinx keyword detection" OFF)
if(V821_AUDIO_BACKEND STREQUAL "alsa")
    find_package(ALSA REQUIRED)
    set(V821_AUDIO_SOURCE "bsp/boards/${BOARD}/audio_pcm.c")
elseif(V821_AUDIO_BACKEND STREQUAL "pipe")
    set(V821_AUDIO_SOURCE "bsp/boards/${BOARD}/audio_alsa.c")
else()
    message(FATAL_ERROR "V821_AUDIO_BACKEND must be pipe or alsa")
endif()

# 只要 SDK 的库，不要它自己的测试和 examples
set(AGENTIC_KIT_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(AGENTIC_KIT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${AK}" "${CMAKE_BINARY_DIR}/_agentic_kit" EXCLUDE_FROM_ALL)

# ---- 设备三元组：构建期从 tuya_authkey.txt 注入 ------------------------------
# 真实的 UUID / AUTH_KEY / PRODUCT_KEY 只存在 tuya_authkey.txt（已 gitignore），
# 这里生成到 build/generated/tuya_authkey.h，**密钥永不进入被提交的源文件**。
# 拷 tuya_authkey.txt.example 成 tuya_authkey.txt 再填。
set(AUTHKEY_TXT "${CMAKE_CURRENT_LIST_DIR}/tuya_authkey.txt")
if(NOT EXISTS "${AUTHKEY_TXT}")
    message(FATAL_ERROR
        "缺少 ${AUTHKEY_TXT}\n"
        "  把 tuya_authkey.txt.example 拷成 tuya_authkey.txt，填上涂鸦平台给的\n"
        "  TUYA_UUID / TUYA_AUTH_KEY / TUYA_PRODUCT_KEY。")
endif()
# 改了 txt 就重新 configure —— 少了这行会一直用旧的生成头
# （ESP 仓库 study-machine 就漏了这个，改完三元组得手动 reconfigure）
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${AUTHKEY_TXT}")

# ★ 故意**不**按行拆成 CMake 列表再 foreach。涂鸦的 secret_key 是随机可打印字符，
# 里面可能出现分号，而 CMake 把分号当列表分隔符：一行会被劈成两段，那个键就
# **静默**解析不到（值里还带 [ 或 ) 时更容易错乱）。所以整体读入、逐个键直接对
# 全文做正则，全程不做列表展开。
file(READ "${AUTHKEY_TXT}" _ak_raw)
string(REPLACE "\r" "" _ak_raw "${_ak_raw}")            # 容忍 CRLF

# 前三个必填；DEMO_* 可选，只给 --demo-dev 跳过配网用（同样是真实设备凭据，
# 所以也只放在这个 gitignored 文件里，不写进源码）。
foreach(_k TUYA_UUID TUYA_AUTH_KEY TUYA_PRODUCT_KEY
           DEMO_DEVID DEMO_SECRET_KEY DEMO_LOCAL_KEY)
    set(AK_${_k} "")
    # 行首必须是键名本身，这样 .example 里注释掉的 "#DEMO_DEVID=" 不会被认成配置
    if(_ak_raw MATCHES "(^|\n)${_k}=([^\n]+)")
        set(_v "${CMAKE_MATCH_2}")
        # 转义成 C 字符串字面量能安全承载的形式（密钥里出现 \ 或 " 也不会炸）
        string(REPLACE "\\" "\\\\" _v "${_v}")
        string(REPLACE "\"" "\\\"" _v "${_v}")
        set(AK_${_k} "${_v}")
    endif()
endforeach()
foreach(_k TUYA_UUID TUYA_AUTH_KEY TUYA_PRODUCT_KEY)
    if(AK_${_k} STREQUAL "")
        message(FATAL_ERROR "${AUTHKEY_TXT} 里缺 ${_k}=")
    endif()
endforeach()
set(_AK_H "${CMAKE_BINARY_DIR}/generated/tuya_authkey.h")
file(WRITE "${_AK_H}"
    "/* 由 CMake 从 tuya_authkey.txt 生成 —— 不要提交，不要手改。 */\n"
    "#ifndef TUYA_AUTHKEY_H\n#define TUYA_AUTHKEY_H\n\n"
    "#define TUYA_UUID        \"${AK_TUYA_UUID}\"\n"
    "#define TUYA_AUTH_KEY    \"${AK_TUYA_AUTH_KEY}\"\n"
    "#define TUYA_PRODUCT_KEY \"${AK_TUYA_PRODUCT_KEY}\"\n\n")
if(NOT AK_DEMO_DEVID STREQUAL "" AND NOT AK_DEMO_SECRET_KEY STREQUAL ""
                                AND NOT AK_DEMO_LOCAL_KEY  STREQUAL "")
    file(APPEND "${_AK_H}"
        "#define HAVE_DEMO_DEV    1\n"
        "#define DEMO_DEVID       \"${AK_DEMO_DEVID}\"\n"
        "#define DEMO_SECRET_KEY  \"${AK_DEMO_SECRET_KEY}\"\n"
        "#define DEMO_LOCAL_KEY   \"${AK_DEMO_LOCAL_KEY}\"\n\n")
    message(STATUS "三元组：已生成；--demo-dev 可用（DEMO_DEVID=${AK_DEMO_DEVID}）")
else()
    message(STATUS "三元组：已生成；--demo-dev 不可用（tuya_authkey.txt 里没配 DEMO_*）")
endif()
file(APPEND "${_AK_H}" "#endif /* TUYA_AUTHKEY_H */\n")

file(GLOB EMOJI_SRCS "${CMAKE_CURRENT_LIST_DIR}/ui/assets/emoji/*.c")
file(GLOB_RECURSE LVGL_SRCS "${LVGL_DIR}/lvgl/src/*.c")

add_executable(v821_chatbot
    app/main.c

    bsp/boards/${BOARD}/board.c
    ${V821_AUDIO_SOURCE}
    bsp/boards/${BOARD}/display_fb.c
    bsp/boards/${BOARD}/camera_v4l2.c

    core/app.c
    core/app_config.c
    core/app_duplex.c
    core/dialog.c
    core/echo_guard.c
    core/pcm_history.c
    core/wake_word.c
    core/audio_dsp.c
    core/presence.c
    core/ringbuf.c
    core/text.c
    core/jsonx.c

    service/session.c
    service/audio_uplink.c
    service/turn_guard.c
    service/provision.c
    service/mcp.c

    ui/ui.c
    ui/ui_qr.c
    ui/assets/fonts/font_puhui_14_1.c
    ${EMOJI_SRCS}

    ${LVGL_SRCS}
)

target_include_directories(v821_chatbot PRIVATE
    "${CMAKE_BINARY_DIR}/generated"              # tuya_authkey.h（构建期生成）
    "${CMAKE_CURRENT_LIST_DIR}"                  # core/ service/ ui/ 互相 include
    "${CMAKE_CURRENT_LIST_DIR}/bsp/include"      # bsp/board.h
    "${CMAKE_CURRENT_LIST_DIR}/bsp/boards/${BOARD}"
    "${CMAKE_CURRENT_LIST_DIR}/ui"               # lv_conf.h / lv_drv_conf.h
    "${LVGL_DIR}" "${LVGL_DIR}/lvgl"
    # qrcodegen 用 LVGL 自带的那份，别再 vendor 一份——符号会撞
    "${LVGL_DIR}/lvgl/src/extra/libs/qrcode"
)
target_compile_definitions(v821_chatbot PRIVATE LV_CONF_INCLUDE_SIMPLE=1)
if(V821_AUDIO_BACKEND STREQUAL "alsa")
    target_compile_definitions(v821_chatbot PRIVATE HAVE_ALSA_PCM=1)
    target_include_directories(v821_chatbot PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_libraries(v821_chatbot PRIVATE ${ALSA_LIBRARIES})
endif()
if(V821_ENABLE_WAKE_WORD)
    set(POCKETSPHINX_ROOT "" CACHE PATH "Target PocketSphinx 5.0.4 installation")
    find_path(POCKETSPHINX_INCLUDE_DIR pocketsphinx.h PATHS "${POCKETSPHINX_ROOT}/include"
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(POCKETSPHINX_LIBRARY NAMES pocketsphinx PATHS "${POCKETSPHINX_ROOT}/lib"
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    if(NOT POCKETSPHINX_INCLUDE_DIR OR NOT POCKETSPHINX_LIBRARY)
        message(FATAL_ERROR "V821_ENABLE_WAKE_WORD needs a target PocketSphinx5.0.4 installation")
    endif()
    target_include_directories(v821_chatbot SYSTEM PRIVATE "${POCKETSPHINX_INCLUDE_DIR}")
    target_link_libraries(v821_chatbot PRIVATE "${POCKETSPHINX_LIBRARY}" m)
    target_compile_definitions(v821_chatbot PRIVATE HAVE_POCKETSPHINX=1)
endif()
target_link_libraries(v821_chatbot PRIVATE
    tuya_ai_tcp tuya_iot_client agentic_kit_pal)

if(EXISTS "${SPEEX_ROOT}/lib/libspeexdsp.a")
    target_include_directories(v821_chatbot PRIVATE "${SPEEX_ROOT}/include")
    target_link_libraries(v821_chatbot PRIVATE "${SPEEX_ROOT}/lib/libspeexdsp.a" m)
else()
    message(FATAL_ERROR "缺少 ${SPEEX_ROOT}/lib/libspeexdsp.a（见 README §2）")
endif()

# libjpeg-turbo：相机软编码。V821 有硬件 JPEG 编码器但要走 MPP，
# 而官方固件没编 MPP 音视频，用不了。
if(EXISTS "${JPEG_ROOT}/lib/libturbojpeg.a")
    target_include_directories(v821_chatbot PRIVATE "${JPEG_ROOT}/include")
    target_link_libraries(v821_chatbot PRIVATE "${JPEG_ROOT}/lib/libturbojpeg.a")
else()
    message(FATAL_ERROR "缺少 ${JPEG_ROOT}/lib/libturbojpeg.a（见 README §2）")
endif()

if(V821_AEC_BACKEND STREQUAL "webrtc")
    if(NOT EXISTS "${WEBRTC_AEC_ROOT}/include/aec_lib.h" OR
       NOT EXISTS "${WEBRTC_AEC_ROOT}/library/musl/libAec.a")
        message(FATAL_ERROR "WEBRTC_AEC_ROOT must contain the SDK target musl library and header")
    endif()
    target_compile_definitions(v821_chatbot PRIVATE HAVE_WEBRTC_AEC=1)
    target_include_directories(v821_chatbot PRIVATE "${WEBRTC_AEC_ROOT}/include")
    target_link_libraries(v821_chatbot PRIVATE "${WEBRTC_AEC_ROOT}/library/musl/libAec.a" m)
endif()

# ---- 本地人形检测（可选）-----------------------------------------------------
# 全志官方预编译的 riscv32+musl 静态库，纯 CPU 软推理（V821 没有 NPU）。
# 来源和拷贝命令见 README §2；缺了就不编这个功能，其余照常。
# 注意它是 C++ 实现（yolo_layer.cpp），所以要用 g++ 链 + 静态带上 libstdc++，
# 否则板上找不到 libstdc++.so。
if(EXISTS "${PDET_ROOT}/lib/libaw_person_det_v821.a")
    target_compile_definitions(v821_chatbot PRIVATE HAVE_PDET=1)
    target_include_directories(v821_chatbot PRIVATE "${PDET_ROOT}/include")
    target_link_libraries(v821_chatbot PRIVATE
        # --start-group：这几个库互相引用，顺序无法一次排对
        -Wl,--start-group
        "${PDET_ROOT}/lib/libnn_p.a"
        "${PDET_ROOT}/lib/libnn_qt_dq_p.a"
        "${PDET_ROOT}/lib/libaw_person_det_nn_v821.a"
        "${PDET_ROOT}/lib/libaw_person_det_v821.a"
        "${PDET_ROOT}/lib/libaw_utilities_standalone_v821.a"
        -Wl,--end-group)
    target_link_options(v821_chatbot PRIVATE -static-libstdc++ -static-libgcc)
    set_target_properties(v821_chatbot PROPERTIES LINKER_LANGUAGE CXX)
    message(STATUS "人形检测：已编入（主动打招呼可用）")
else()
    message(STATUS "人形检测：未编入（缺 ${PDET_ROOT}），主动打招呼功能关闭")
endif()

# ---- 早期连云验证用的小工具 --------------------------------------------------
add_executable(activate_demo
    "${AK_POSIX}/pair/api-activate/activate_demo.c"
    "${AK_POSIX}/pair/api-activate/activate_demo_main.c")
target_include_directories(activate_demo PRIVATE "${AK_POSIX}/pair/api-activate")
target_link_libraries(activate_demo PRIVATE tuya_iot_client)

add_executable(tai_text_chat_demo "${AK_POSIX}/ai/rtc-tcp-client/text_chat_demo.c")
target_link_libraries(tai_text_chat_demo PRIVATE
    tuya_ai_tcp tuya_iot_client agentic_kit_pal)
