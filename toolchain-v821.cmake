# CMake toolchain file — 九鼎 V821 (Allwinner V821) / Tina Linux 5.0
#
# Target: riscv32, musl libc, ilp32d ABI, Andes extension.
# 工具链默认的 -march/-mabi 就已经与板子固件的 ELF 属性逐字段一致
# (rv32i2p0_m2p0_a2p0_f2p0_d2p0_c2p0_xandes5p0 / ilp32d)，所以这里
# 故意不显式设置 march/mabi —— 一旦写错会与 sysroot 里的 libc 不匹配。
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv32)

# 工具链位置。按你的实际路径改，或在命令行传：
#   cmake ... -DV821_TOOLCHAIN=/path/to/nds32le-linux-musl-v5d
# 也可以设环境变量 V821_TOOLCHAIN。解压方式见 README §2。
if(NOT V821_TOOLCHAIN)
    if(DEFINED ENV{V821_TOOLCHAIN})
        set(V821_TOOLCHAIN "$ENV{V821_TOOLCHAIN}")
    else()
        set(V821_TOOLCHAIN "$ENV{HOME}/kit/v821-tina-v12/out/toolchain/nds32le-linux-musl-v5d")
    endif()
endif()
if(NOT EXISTS "${V821_TOOLCHAIN}/bin/riscv32-linux-musl-gcc")
    message(FATAL_ERROR
        "找不到 V821 工具链：${V821_TOOLCHAIN}\n"
        "  用 -DV821_TOOLCHAIN=<路径> 指定，或设同名环境变量。\n"
        "  它在九鼎 SDK 里：prebuilt/rootfsbuilt/riscv/nds32le-linux-musl-v5d.tar.xz（见 README §2）")
endif()

set(V821_TOOLCHAIN "${V821_TOOLCHAIN}" CACHE PATH "V821 riscv32-musl toolchain directory")

set(CMAKE_C_COMPILER   "${V821_TOOLCHAIN}/bin/riscv32-linux-musl-gcc")
set(CMAKE_CXX_COMPILER "${V821_TOOLCHAIN}/bin/riscv32-linux-musl-g++")
set(CMAKE_SYSROOT      "${V821_TOOLCHAIN}/sysroot")

# 交叉编译查找规则：程序在 host 上找（cmake 顶层要 find_package(Python3)），
# 库和头文件只在 sysroot 里找，绝不误用宿主机的 x86 库。
set(CMAKE_FIND_ROOT_PATH "${V821_TOOLCHAIN}/sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# musl 的 pthread 在 libc 里，但 FindThreads 仍需 -pthread 试编译
set(THREADS_PREFER_PTHREAD_FLAG ON)
