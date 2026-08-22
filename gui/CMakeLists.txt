cmake_minimum_required(VERSION 3.21)

# Windows 下与仓库内预编译的 LuaJIT/Keystone 保持相同的静态 CRT。
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
option(MINIMEM_STATIC_RUNTIME "Windows 使用静态 MSVC CRT (/MT)" ON)
if(WIN32 AND MINIMEM_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded"
        CACHE STRING "Windows CRT 运行库")
endif()

# ==================== 项目版本定义 ====================
set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_PATCH 2)
set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

# 协议版本（与设备端协商使用）
set(PROTOCOL_VERSION_MAJOR 1)
set(PROTOCOL_VERSION_MINOR 0)
set(PROTOCOL_VERSION_PATCH 2)
set(PROTOCOL_VERSION "${PROTOCOL_VERSION_MAJOR}.${PROTOCOL_VERSION_MINOR}.${PROTOCOL_VERSION_PATCH}")

project(MiniMemClient VERSION ${PROJECT_VERSION})
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)

# 渲染后端选项
if(WIN32)
    option(USE_DX12 "Use DirectX 12 backend" ON)
    option(USE_OPENGL3 "Use GLFW + OpenGL3 backend" OFF)
else()
    option(USE_DX12 "Use DirectX 12 backend" OFF)
    option(USE_OPENGL3 "Use GLFW + OpenGL3 backend" ON)
endif()

if(USE_DX12 AND USE_OPENGL3)
    message(FATAL_ERROR "USE_DX12 与 USE_OPENGL3 不能同时启用")
elseif(NOT USE_DX12 AND NOT USE_OPENGL3)
    message(FATAL_ERROR "必须启用一个窗口渲染后端：USE_DX12 或 USE_OPENGL3")
endif()

# 生成版本信息头文件
string(TIMESTAMP BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP BUILD_TIME "%H:%M:%S")

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
else()
    set(GIT_COMMIT_HASH "unknown")
    set(GIT_BRANCH "unknown")
endif()

configure_file(${CMAKE_SOURCE_DIR}/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)

# 输出目录可由 CI/上层项目覆盖，本地构建仍保持原有目录布局。
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
endif()
if(NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
endif()

# ==================== 可选：Capstone（Lua 反汇编） ====================
option(MINIMEM_ENABLE_CAPSTONE "启用 Lua Capstone 反汇编支持" ON)
option(MINIMEM_REQUIRE_CAPSTONE "找不到 Capstone 时使配置失败" OFF)
set(capstone_ROOT "" CACHE PATH "Capstone 安装目录")
set(_MINIMEM_VCPKG_STATIC_ROOT
    "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows-static")

if(MINIMEM_ENABLE_CAPSTONE)
    find_path(CAPSTONE_INCLUDE_DIR NAMES capstone/capstone.h
        HINTS "${capstone_ROOT}/include" "${_MINIMEM_VCPKG_STATIC_ROOT}/include")
    find_library(CAPSTONE_LIBRARY NAMES capstone capstone_static
        HINTS "${capstone_ROOT}/lib" "${_MINIMEM_VCPKG_STATIC_ROOT}/lib")
endif()

if(MINIMEM_ENABLE_CAPSTONE AND CAPSTONE_INCLUDE_DIR AND CAPSTONE_LIBRARY)
    set(HAVE_CAPSTONE TRUE)
    set(CAPSTONE_LIBRARIES ${CAPSTONE_LIBRARY})
    set(CAPSTONE_INCLUDE_DIRS ${CAPSTONE_INCLUDE_DIR})
elseif(MINIMEM_REQUIRE_CAPSTONE)
    message(FATAL_ERROR "Capstone 未找到。请设置 capstone_ROOT 或安装静态 vcpkg 包。")
else()
    set(HAVE_CAPSTONE FALSE)
    message(WARNING "Capstone 未找到：Lua 反汇编功能将被禁用。")
endif()

# ==================== 可选：Keystone（Lua 汇编） ====================
option(MINIMEM_ENABLE_KEYSTONE "启用 Lua Keystone 汇编支持" ON)
option(MINIMEM_REQUIRE_KEYSTONE "找不到 Keystone 时使配置失败" OFF)
if(WIN32)
    set(_KEYSTONE_DEFAULT_ROOT "${_MINIMEM_VCPKG_STATIC_ROOT}")
else()
    set(_KEYSTONE_DEFAULT_ROOT "${CMAKE_SOURCE_DIR}/third_party/keystone-linux")
endif()
set(keystone_ROOT "${_KEYSTONE_DEFAULT_ROOT}" CACHE PATH "Keystone installation directory")
if(MINIMEM_ENABLE_KEYSTONE)
    find_path(KEYSTONE_INCLUDE_DIR keystone/keystone.h
        HINTS "${keystone_ROOT}/include" "${keystone_ROOT}" PATH_SUFFIXES include)
    find_library(KEYSTONE_LIBRARY NAMES keystone
        HINTS "${keystone_ROOT}/lib" "${keystone_ROOT}" PATH_SUFFIXES lib lib64)
endif()
if(MINIMEM_ENABLE_KEYSTONE AND KEYSTONE_INCLUDE_DIR AND KEYSTONE_LIBRARY)
    set(HAVE_KEYSTONE TRUE)
    set(KEYSTONE_LIBRARIES ${KEYSTONE_LIBRARY})
    set(KEYSTONE_INCLUDE_DIRS ${KEYSTONE_INCLUDE_DIR})
elseif(MINIMEM_REQUIRE_KEYSTONE)
    message(FATAL_ERROR "Keystone 未找到。请设置 keystone_ROOT 或安装静态 vcpkg 包。")
else()
    set(HAVE_KEYSTONE FALSE)
    message(WARNING "Keystone 未找到：Lua 汇编功能将被禁用。")
endif()

# ==================== 源文件集合 ====================
set(IMGUI_SOURCES
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp
)

# GUI：精简版仅保留 主窗口/服务器连接/模块列表/日志/版本 + 反汇编辅助(供 Lua)
set(GUI_SOURCES
    gui/Gui.cpp
    gui/Window.cpp
    gui/CEWindow.cpp
    gui/ServerConnectWindow.cpp
    gui/ModulesWindow.cpp
    gui/UxnBreakpointWindow.cpp
    gui/VersionWindow.cpp
    gui/DisassemblyHelper.cpp
    gui/AssemblyHelper.cpp
    gui/ConfigManager.cpp
    gui/LogWindow.cpp
    gui/AppContext.cpp
)

# Lua 脚本引擎（用于复杂分析）
set(LUA_SOURCES)

# 查找 LuaJIT（可选）。Linux 优先使用 OpenResty luajit2 包；
# Ubuntu 的 libluajit2-5.1-dev 提供 pkg-config 名称 luajit 和库名 luajit-5.1。
option(LUAJIT_STATIC "Link LuaJIT statically" ON)
option(MINIMEM_REQUIRE_LUAJIT "找不到 LuaJIT 时使配置失败" OFF)
set(LUAJIT_DIR "${CMAKE_SOURCE_DIR}/third_party/LuaJIT"
    CACHE PATH "LuaJIT 安装目录")
set(LUAJIT_INCLUDE "${LUAJIT_DIR}/include")
set(LUAJIT_LIB "${LUAJIT_DIR}/lib")
set(LUAJIT_BIN "${LUAJIT_DIR}/bin")

set(HAVE_LUAJIT FALSE)
set(_LUAJIT_HAS_HEADERS FALSE)
if(EXISTS "${LUAJIT_INCLUDE}/lua.hpp" OR EXISTS "${LUAJIT_INCLUDE}/luajit.h" OR EXISTS "${LUAJIT_INCLUDE}/lua.h")
    set(_LUAJIT_HAS_HEADERS TRUE)
endif()

if(_LUAJIT_HAS_HEADERS)
    set(_LUAJIT_LOCAL_NAMES luajit-5.1 luajit)
    if(WIN32)
        list(PREPEND _LUAJIT_LOCAL_NAMES lua51)
    endif()
    find_library(LUAJIT_LIBRARY
        NAMES ${_LUAJIT_LOCAL_NAMES}
        HINTS "${LUAJIT_LIB}"
        NO_DEFAULT_PATH)
    if(LUAJIT_LIBRARY)
        set(HAVE_LUAJIT TRUE)
        set(LUAJIT_IS_STATIC ${LUAJIT_STATIC})
    endif()
endif()

if(NOT HAVE_LUAJIT)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PC_LUAJIT QUIET IMPORTED_TARGET luajit)
        if(NOT PC_LUAJIT_FOUND)
            pkg_check_modules(PC_LUAJIT QUIET IMPORTED_TARGET luajit2)
        endif()
        if(NOT PC_LUAJIT_FOUND)
            pkg_check_modules(PC_LUAJIT QUIET IMPORTED_TARGET luajit-5.1)
        endif()
    endif()
endif()

if(PC_LUAJIT_FOUND)
    set(HAVE_LUAJIT TRUE)
    set(LUAJIT_INCLUDE ${PC_LUAJIT_INCLUDE_DIRS})
    set(LUAJIT_LIBRARY PkgConfig::PC_LUAJIT)
    set(LUAJIT_IS_STATIC OFF)
elseif(HAVE_LUAJIT)
    set(LUAJIT_IS_STATIC ${LUAJIT_STATIC})
elseif(MINIMEM_REQUIRE_LUAJIT)
    message(FATAL_ERROR "LuaJIT 未找到。请设置 LUAJIT_DIR 或安装 luajit pkg-config 包。")
else()
    message(WARNING "LuaJIT 未找到：Lua 脚本窗口与 Lua IPC 方法将被禁用。")
endif()

if(HAVE_LUAJIT)
    list(APPEND GUI_SOURCES gui/LuaScriptWindow.cpp gui/LuaImGuiWindow.cpp)
    list(APPEND LUA_SOURCES
        lua/LuaDiagnostics.cpp
        lua/LuaEngine.cpp
        lua/LuaAPI.cpp
        lua/LuaAPI_Memory.cpp
        lua/LuaAPI_Uxn.cpp
        lua/LuaAPI_ImGui.cpp
        lua/LuaAPI_Assembly.cpp
    )
endif()

# Socket 协议层：精简版无搜索/冻结命令
set(SOCKET_SOURCES
    socket/DeviceSession.cpp
    socket/client_singleton.cpp
    socket/ProcessCommands.cpp
    socket/MemoryCommands.cpp
    socket/BreakpointCommands.cpp
    socket/UxnBreakpointCommands.cpp
    socket/SymbolCommands.cpp
)

set(MEM_SOURCES
    mem/MemService.cpp
    mem/SystemMemService.cpp
)

set(RENDERER_SOURCES
    renderer/StyleSetup.cpp
)

set(IPC_SOURCES
    ipc/IpcHttpRequest.cpp
    ipc/IpcServer.cpp
)

# 配置渲染/窗口后端
if(USE_DX12)
    if(NOT WIN32)
        message(FATAL_ERROR "DirectX 12 后端仅支持 Windows；Linux 请使用 USE_OPENGL3=ON。")
    endif()
    list(APPEND IMGUI_SOURCES imgui/backends/imgui_impl_dx12.cpp)
    list(APPEND IMGUI_SOURCES imgui/backends/imgui_impl_win32.cpp)
    list(APPEND RENDERER_SOURCES renderer/DX12Renderer.cpp renderer/AppWindowWin32.cpp)
    set(PLATFORM_LIBS d3d12 dxgi d3dcompiler ws2_32)
    set(PLATFORM_DEFINITION USE_DX12)
elseif(USE_OPENGL3)
    if(WIN32)
        message(FATAL_ERROR "OpenGL3 后端仅用于 Linux/macOS；Windows 请使用 USE_DX12=ON。")
    endif()
    find_package(Threads REQUIRED)
    find_package(OpenGL QUIET)
    if(NOT OpenGL_FOUND)
        message(FATAL_ERROR "未找到 OpenGL 开发包。Linux 请安装 libgl1-mesa-dev（Debian/Ubuntu）或 mesa-libGL-devel（Fedora）。")
    endif()
    find_package(glfw3 CONFIG QUIET)
    if(NOT glfw3_FOUND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(GLFW3 QUIET IMPORTED_TARGET glfw3)
        endif()
    endif()
    if(NOT glfw3_FOUND AND NOT GLFW3_FOUND)
        message(FATAL_ERROR "未找到 GLFW3。Linux 请安装 libglfw3-dev（Debian/Ubuntu）或 glfw-devel（Fedora）。")
    endif()

    list(APPEND IMGUI_SOURCES imgui/backends/imgui_impl_glfw.cpp)
    list(APPEND IMGUI_SOURCES imgui/backends/imgui_impl_opengl3.cpp)
    list(APPEND RENDERER_SOURCES renderer/AppWindowGLFW.cpp)
    set(PLATFORM_LIBS OpenGL::GL Threads::Threads ${CMAKE_DL_LIBS})
    if(TARGET glfw)
        list(APPEND PLATFORM_LIBS glfw)
    elseif(TARGET glfw3::glfw)
        list(APPEND PLATFORM_LIBS glfw3::glfw)
    else()
        list(APPEND PLATFORM_LIBS PkgConfig::GLFW3)
    endif()
    set(PLATFORM_DEFINITION USE_OPENGL3)
else()
    message(FATAL_ERROR "必须启用一个窗口/渲染后端：USE_DX12 或 USE_OPENGL3。")
endif()

add_executable(MiniMemClient
    main.cpp
    ${IMGUI_SOURCES}
    ${GUI_SOURCES}
    ${SOCKET_SOURCES}
    ${MEM_SOURCES}
    ${RENDERER_SOURCES}
    ${IPC_SOURCES}
    ${LUA_SOURCES}
)

target_compile_definitions(MiniMemClient PRIVATE
    IMGUI_DISABLE_DEBUG_TOOLS
    ${PLATFORM_DEFINITION}
)

if(MSVC)
    target_compile_options(MiniMemClient PRIVATE /utf-8)
endif()

# 包含目录
target_include_directories(MiniMemClient PRIVATE
    imgui imgui/backends gui lua renderer utils ipc mem socket
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_BINARY_DIR}/generated
)

if(HAVE_CAPSTONE)
    target_include_directories(MiniMemClient PRIVATE ${CAPSTONE_INCLUDE_DIRS})
endif()
if(HAVE_KEYSTONE)
    target_include_directories(MiniMemClient PRIVATE ${KEYSTONE_INCLUDE_DIRS})
endif()

if(BUILD_TESTING)
    add_executable(minimem_mem_service_tests
        tests/MemServiceTests.cpp
        mem/MemService.cpp
    )
    target_include_directories(minimem_mem_service_tests PRIVATE mem)
    add_test(NAME minimem_mem_service
             COMMAND minimem_mem_service_tests)

    add_executable(minimem_device_session_tests
        tests/DeviceSessionTests.cpp
        socket/DeviceSession.cpp
    )
    target_include_directories(minimem_device_session_tests PRIVATE socket)
    add_test(NAME minimem_device_session
             COMMAND minimem_device_session_tests)

    add_executable(minimem_config_security_tests
        tests/ConfigSecurityTests.cpp
        gui/ConfigManager.cpp
    )
    target_include_directories(minimem_config_security_tests PRIVATE gui)
    add_test(NAME minimem_config_security
             COMMAND minimem_config_security_tests)

    add_executable(minimem_ipc_http_request_tests
        tests/IpcHttpRequestTests.cpp
        ipc/IpcHttpRequest.cpp
    )
    target_include_directories(minimem_ipc_http_request_tests PRIVATE ipc)
    add_test(NAME minimem_ipc_http_request
             COMMAND minimem_ipc_http_request_tests)

    add_executable(minimem_socket_safety_tests
        tests/SocketSafetyTests.cpp
    )
    target_include_directories(minimem_socket_safety_tests PRIVATE socket)
    if(WIN32)
        target_link_libraries(minimem_socket_safety_tests PRIVATE ws2_32)
    else()
        target_link_libraries(minimem_socket_safety_tests PRIVATE Threads::Threads)
    endif()
    add_test(NAME minimem_socket_safety
             COMMAND minimem_socket_safety_tests)

    if(HAVE_LUAJIT)
        add_executable(minimem_lua_sandbox_tests
            tests/LuaSandboxTests.cpp
            imgui/imgui.cpp
            imgui/imgui_draw.cpp
            imgui/imgui_tables.cpp
            imgui/imgui_widgets.cpp
            gui/DisassemblyHelper.cpp
            gui/AssemblyHelper.cpp
            gui/LuaImGuiWindow.cpp
            ${LUA_SOURCES}
        )
        target_include_directories(minimem_lua_sandbox_tests PRIVATE
            imgui gui lua mem socket
            ${CMAKE_SOURCE_DIR}/third_party
            ${LUAJIT_INCLUDE}
        )
        target_compile_definitions(minimem_lua_sandbox_tests PRIVATE
            HAVE_LUAJIT IMGUI_DISABLE_DEBUG_TOOLS)
        if(LUAJIT_IS_STATIC)
            target_compile_definitions(minimem_lua_sandbox_tests PRIVATE
                LUAJIT_STATIC)
        endif()
        if(MSVC)
            target_compile_options(minimem_lua_sandbox_tests PRIVATE /utf-8)
        endif()
        target_link_libraries(minimem_lua_sandbox_tests PRIVATE
            ${LUAJIT_LIBRARY})
        if(NOT WIN32)
            target_link_libraries(minimem_lua_sandbox_tests PRIVATE Threads::Threads)
        endif()
        if(WIN32)
            add_test(NAME minimem_lua_sandbox
                     COMMAND minimem_lua_sandbox_tests)
            set_tests_properties(minimem_lua_sandbox PROPERTIES TIMEOUT 10)
        endif()
    endif()

    add_test(
        NAME minimem_mem_service_boundary
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/tests/VerifyMemServiceBoundary.cmake)
endif()
if(HAVE_LUAJIT)
    target_include_directories(MiniMemClient PRIVATE ${LUAJIT_INCLUDE})
endif()

# 链接系统库
target_link_libraries(MiniMemClient PRIVATE ${PLATFORM_LIBS})

# 链接 LuaJIT
if(HAVE_LUAJIT)
    target_link_libraries(MiniMemClient PRIVATE ${LUAJIT_LIBRARY})
    target_compile_definitions(MiniMemClient PRIVATE HAVE_LUAJIT)
    if(LUAJIT_IS_STATIC)
        target_compile_definitions(MiniMemClient PRIVATE LUAJIT_STATIC)
    elseif(EXISTS "${LUAJIT_BIN}/lua51.dll")
        add_custom_command(TARGET MiniMemClient POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LUAJIT_BIN}/lua51.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lua51.dll")
    endif()
endif()

# 链接 Capstone
if(HAVE_CAPSTONE)
    target_link_libraries(MiniMemClient PRIVATE ${CAPSTONE_LIBRARIES})
    target_compile_definitions(MiniMemClient PRIVATE HAVE_CAPSTONE)
    get_filename_component(_capstone_lib_dir ${CAPSTONE_LIBRARY} DIRECTORY)
    get_filename_component(_capstone_bin_dir "${_capstone_lib_dir}/../bin" ABSOLUTE)
    if(EXISTS "${_capstone_bin_dir}/capstone.dll")
        add_custom_command(TARGET MiniMemClient POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_capstone_bin_dir}/capstone.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/capstone.dll")
    endif()
endif()

# 链接 Keystone
if(HAVE_KEYSTONE)
    target_link_libraries(MiniMemClient PRIVATE ${KEYSTONE_LIBRARIES})
    target_compile_definitions(MiniMemClient PRIVATE HAVE_KEYSTONE)
    get_filename_component(_keystone_lib_dir ${KEYSTONE_LIBRARY} DIRECTORY)
    if(WIN32)
        get_filename_component(_keystone_bin_dir "${_keystone_lib_dir}/../bin" ABSOLUTE)
    endif()
    if(WIN32 AND EXISTS "${_keystone_bin_dir}/keystone.dll")
        add_custom_command(TARGET MiniMemClient POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_keystone_bin_dir}/keystone.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/keystone.dll")
    elseif(UNIX AND NOT APPLE AND EXISTS "${_keystone_lib_dir}/libkeystone.so.0")
        set_property(TARGET MiniMemClient APPEND PROPERTY
            BUILD_RPATH "$ORIGIN;${_keystone_lib_dir}")
        set_property(TARGET MiniMemClient APPEND PROPERTY
            INSTALL_RPATH "$ORIGIN")
        add_custom_command(TARGET MiniMemClient POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_keystone_lib_dir}/libkeystone.so.0"
            "$<TARGET_FILE_DIR:MiniMemClient>/libkeystone.so.0")
    endif()
endif()
