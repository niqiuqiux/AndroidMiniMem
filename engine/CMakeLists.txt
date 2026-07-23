cmake_minimum_required(VERSION 3.21)

# ============================================================================
# Android MiniMem Engine - CMake 配置
# ----------------------------------------------------------------------------
# 精简版内存引擎：仅保留 进程/模块列表、内存读写、硬件断点、ELF符号、内核切换。
# 已剥离：数据搜索 / 指针扫描 / 冻结列表 / SO注入 / 多余接口(pipe/stdio/uds/jni)。
# ============================================================================

# ----------------------------------------------------------------------------
# NDK 工具链。支持标准 CMAKE_TOOLCHAIN_FILE，也兼容现有的 -DANDROID_NDK=。
# ----------------------------------------------------------------------------
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(_MINIMEM_NDK "${ANDROID_NDK}")
    if(NOT _MINIMEM_NDK AND DEFINED ENV{ANDROID_NDK_HOME})
        set(_MINIMEM_NDK "$ENV{ANDROID_NDK_HOME}")
    elseif(NOT _MINIMEM_NDK AND DEFINED ENV{ANDROID_NDK_ROOT})
        set(_MINIMEM_NDK "$ENV{ANDROID_NDK_ROOT}")
    elseif(NOT _MINIMEM_NDK AND DEFINED ENV{ANDROID_NDK})
        set(_MINIMEM_NDK "$ENV{ANDROID_NDK}")
    endif()

    if(NOT _MINIMEM_NDK)
        message(FATAL_ERROR
            "未指定 Android NDK。请设置 ANDROID_NDK_HOME，或传入 "
            "-DANDROID_NDK=<path> / -DCMAKE_TOOLCHAIN_FILE=<path>。")
    endif()

    set(_MINIMEM_TOOLCHAIN "${_MINIMEM_NDK}/build/cmake/android.toolchain.cmake")
    if(NOT EXISTS "${_MINIMEM_TOOLCHAIN}")
        message(FATAL_ERROR "Android NDK 工具链不存在：${_MINIMEM_TOOLCHAIN}")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "${_MINIMEM_TOOLCHAIN}" CACHE FILEPATH
        "Android NDK CMake toolchain")
endif()

set(ANDROID_ABI "arm64-v8a" CACHE STRING "Android ABI")
set(ANDROID_PLATFORM "android-26" CACHE STRING "Android API level")

# ============================================================================
# 项目定义
# ============================================================================
project(android_minimem_engine
    VERSION 1.0.0
    LANGUAGES C CXX
    DESCRIPTION "Android MiniMem Engine (进程/模块/读写/断点/内核切换)"
)

if(NOT ANDROID)
    message(FATAL_ERROR "android_minimem_engine 只能使用 Android NDK 工具链构建")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# ----------------------------------------------------------------------------
# 构建选项
# ----------------------------------------------------------------------------
option(BUILD_SOCKET_INTERFACE "构建 Socket 接口" ON)
option(ENABLE_STRIP "构建后自动 strip 符号" ON)

# ----------------------------------------------------------------------------
# 输出目录
# ----------------------------------------------------------------------------
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
endif()
if(NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
endif()

# ----------------------------------------------------------------------------
# C/C++ 标准与编译选项
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ----------------------------------------------------------------------------
# 公共接口库（共享 include 路径与系统库）
# ----------------------------------------------------------------------------
add_library(common_interface INTERFACE)

target_include_directories(common_interface INTERFACE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/android
    ${CMAKE_SOURCE_DIR}/ceserver
)

target_compile_definitions(common_interface INTERFACE
    ANDROID_CHEAT_ENGINE
)

target_compile_options(common_interface INTERFACE
    -fvisibility=hidden
    -Wno-format
)

target_link_libraries(common_interface INTERFACE
    android
    z
    log
)

# ----------------------------------------------------------------------------
# 服务器核心静态库
#   CE 协议分发(CEServer/porthelp) + API 实现(api) + 内核读写驱动(AndroidKernelDriver)
# ----------------------------------------------------------------------------
add_library(server_core STATIC
    ${CMAKE_SOURCE_DIR}/ceserver/CEServer.cpp
    ${CMAKE_SOURCE_DIR}/ceserver/porthelp.cpp
    ${CMAKE_SOURCE_DIR}/ceserver/api.cpp
    ${CMAKE_SOURCE_DIR}/android/NiDriver.cpp
    ${CMAKE_SOURCE_DIR}/android/AndroidKernelDriver.cpp
)

target_link_libraries(server_core PUBLIC common_interface)

# ----------------------------------------------------------------------------
# Strip 辅助函数
# ----------------------------------------------------------------------------
function(apply_strip target)
    if(ENABLE_STRIP AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_STRIP} --strip-all --remove-section=.comment
                    "$<TARGET_FILE:${target}>"
            COMMENT "剥离 ${target} 的符号表..."
        )
    endif()
endfunction()

# ----------------------------------------------------------------------------
# 子目录
# ----------------------------------------------------------------------------
add_subdirectory(interface)

# ----------------------------------------------------------------------------
# 构建信息
# ----------------------------------------------------------------------------
message(STATUS "========================================")
message(STATUS "Android MiniMem Engine 构建配置")
message(STATUS "========================================")
message(STATUS "NDK 路径: ${CMAKE_ANDROID_NDK}")
message(STATUS "Android 平台: ${ANDROID_PLATFORM}  ABI: ${ANDROID_ABI}")
message(STATUS "C++ 标准: ${CMAKE_CXX_STANDARD}  构建类型: ${CMAKE_BUILD_TYPE}")
message(STATUS "Socket 接口: ${BUILD_SOCKET_INTERFACE}")
message(STATUS "Strip 符号: ${ENABLE_STRIP}")
message(STATUS "========================================")
