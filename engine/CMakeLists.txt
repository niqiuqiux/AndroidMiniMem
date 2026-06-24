cmake_minimum_required(VERSION 3.18)

# ============================================================================
# Android MiniMem Engine - CMake 配置
# ----------------------------------------------------------------------------
# 精简版内存引擎：仅保留 进程/模块列表、内存读写、硬件断点、ELF符号、内核切换。
# 已剥离：数据搜索 / 指针扫描 / 冻结列表 / SO注入 / 多余接口(pipe/stdio/uds/jni)。
# ============================================================================

if(UNIX AND NOT APPLE)
    set(LINUX_HOME_DIR "$ENV{HOME}")
    if(LINUX_HOME_DIR STREQUAL "/home/ni")
        set(DEFAULT_ANDROID_NDK_PATH "/home/ni/android-ndk-r27c")
    elseif(LINUX_HOME_DIR STREQUAL "/home/qiu")
        set(DEFAULT_ANDROID_NDK_PATH "/home/qiu/Android/android-ndk-r28c")
    else()
        set(DEFAULT_ANDROID_NDK_PATH "${LINUX_HOME_DIR}/Android/android-ndk-r28c")
    endif()
endif()

# ----------------------------------------------------------------------------
# NDK 路径配置（优先环境变量 ANDROID_NDK，否则默认路径，可用 -DANDROID_NDK= 覆盖）
# ----------------------------------------------------------------------------
if(NOT DEFINED ANDROID_NDK)
    if(WIN32)
        set(CMAKE_ANDROID_NDK "D:/ndkollvm/android-ndk-r27c-windows/android-ndk-r27c"
            CACHE PATH "Android NDK path")
    else()
        set(CMAKE_ANDROID_NDK "${DEFAULT_ANDROID_NDK_PATH}"
            CACHE PATH "Android NDK path")
    endif()
else()
    set(CMAKE_ANDROID_NDK ${ANDROID_NDK})
endif()

set(CMAKE_SYSTEM_NAME ANDROID)
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_ANDROID_NDK}/build/cmake/android.toolchain.cmake)

set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
set(ANDROID_ABI arm64-v8a)
set(ANDROID_PLATFORM android-26)
set(ANDROID_NDK ${CMAKE_ANDROID_NDK})

# ============================================================================
# 项目定义
# ============================================================================
project(android_minimem_engine
    VERSION 1.0.0
    LANGUAGES C CXX
    DESCRIPTION "Android MiniMem Engine (进程/模块/读写/断点/内核切换)"
)

# ----------------------------------------------------------------------------
# 构建选项
# ----------------------------------------------------------------------------
option(BUILD_SOCKET_INTERFACE "构建 Socket 接口" ON)
option(ENABLE_OBFUSCATION "启用代码混淆" OFF)
option(ENABLE_STRIP "构建后自动 strip 符号" ON)

# ----------------------------------------------------------------------------
# 输出目录
# ----------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)

# ----------------------------------------------------------------------------
# C/C++ 标准与编译选项
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(COMMON_COMPILE_FLAGS "-fvisibility=hidden -Wno-format")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_COMPILE_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_COMPILE_FLAGS}")

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

target_link_libraries(common_interface INTERFACE
    android
    z
    log
)

# ----------------------------------------------------------------------------
# 服务器核心静态库
#   CE 协议分发(CEServer/porthelp) + API 实现(api) + 内核读写驱动(MemoryReaderWriter)
# ----------------------------------------------------------------------------
add_library(server_core STATIC
    ${CMAKE_SOURCE_DIR}/ceserver/CEServer.cpp
    ${CMAKE_SOURCE_DIR}/ceserver/porthelp.cpp
    ${CMAKE_SOURCE_DIR}/ceserver/api.cpp
    ${CMAKE_SOURCE_DIR}/android/MemoryReaderWriter.cpp
)

target_link_libraries(server_core PUBLIC common_interface)

# ----------------------------------------------------------------------------
# 混淆 / Strip 辅助函数（保留接口，便于 release 构建）
# ----------------------------------------------------------------------------
function(apply_obfuscation target)
    # MiniMem 默认不启用混淆；如需可在此接入 LLVM 混淆插件
endfunction()

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
