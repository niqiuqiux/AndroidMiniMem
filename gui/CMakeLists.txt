cmake_minimum_required(VERSION 3.16)

# ==================== 项目版本定义 ====================
set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_PATCH 0)
set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

# 协议版本（与设备端协商使用）
set(PROTOCOL_VERSION_MAJOR 1)
set(PROTOCOL_VERSION_MINOR 0)
set(PROTOCOL_VERSION_PATCH 0)
set(PROTOCOL_VERSION "${PROTOCOL_VERSION_MAJOR}.${PROTOCOL_VERSION_MINOR}.${PROTOCOL_VERSION_PATCH}")

project(MiniMemClient VERSION ${PROJECT_VERSION})
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 渲染后端选项
if(WIN32)
    option(USE_DX12 "Use DirectX 12 backend" ON)
    option(USE_OPENGL3 "Use GLFW + OpenGL3 backend" OFF)
else()
    option(USE_DX12 "Use DirectX 12 backend" OFF)
    option(USE_OPENGL3 "Use GLFW + OpenGL3 backend" ON)
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

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)

# ==================== 可选：Capstone（Lua 反汇编） ====================
set(capstone_ROOT "C:/Program Files/capstone" CACHE PATH "Capstone installation directory")
find_package(capstone CONFIG QUIET)
if(capstone_FOUND)
    find_library(CAPSTONE_LIBRARY NAMES capstone capstone_static
        HINTS "${capstone_ROOT}/lib" "${capstone_ROOT}/build" "${capstone_ROOT}" PATH_SUFFIXES lib lib64)
    find_path(CAPSTONE_INCLUDE_DIR NAMES capstone/capstone.h
        HINTS "${capstone_ROOT}/include" "${capstone_ROOT}" PATH_SUFFIXES include)
else()
    find_path(CAPSTONE_INCLUDE_DIR capstone/capstone.h)
    find_library(CAPSTONE_LIBRARY NAMES capstone)
endif()
if(CAPSTONE_INCLUDE_DIR AND CAPSTONE_LIBRARY)
    set(HAVE_CAPSTONE TRUE)
    set(CAPSTONE_LIBRARIES ${CAPSTONE_LIBRARY})
    set(CAPSTONE_INCLUDE_DIRS ${CAPSTONE_INCLUDE_DIR})
else()
    set(HAVE_CAPSTONE FALSE)
    message(WARNING "Capstone 未找到：Lua 反汇编功能将被禁用。")
endif()

# ==================== 可选：Keystone（Lua 汇编） ====================
if(WIN32)
    set(_KEYSTONE_DEFAULT_ROOT "C:/vcpkg/installed/x64-windows")
else()
    set(_KEYSTONE_DEFAULT_ROOT "${CMAKE_SOURCE_DIR}/third_party/keystone-linux")
endif()
set(keystone_ROOT "${_KEYSTONE_DEFAULT_ROOT}" CACHE PATH "Keystone installation directory")
find_path(KEYSTONE_INCLUDE_DIR keystone/keystone.h
    HINTS "${keystone_ROOT}/include" "${keystone_ROOT}" PATH_SUFFIXES include)
find_library(KEYSTONE_LIBRARY NAMES keystone
    HINTS "${keystone_ROOT}/lib" "${keystone_ROOT}" PATH_SUFFIXES lib lib64)
if(KEYSTONE_INCLUDE_DIR AND KEYSTONE_LIBRARY)
    set(HAVE_KEYSTONE TRUE)
    set(KEYSTONE_LIBRARIES ${KEYSTONE_LIBRARY})
    set(KEYSTONE_INCLUDE_DIRS ${KEYSTONE_INCLUDE_DIR})
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
set(LUAJIT_DIR "${CMAKE_SOURCE_DIR}/third_party/LuaJIT")
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
        pkg_check_modules(PC_LUAJIT QUIET luajit)
        if(NOT PC_LUAJIT_FOUND)
            pkg_check_modules(PC_LUAJIT QUIET luajit2)
        endif()
        if(NOT PC_LUAJIT_FOUND)
            pkg_check_modules(PC_LUAJIT QUIET luajit-5.1)
        endif()
    endif()
endif()

if(PC_LUAJIT_FOUND)
    set(HAVE_LUAJIT TRUE)
    set(LUAJIT_INCLUDE ${PC_LUAJIT_INCLUDE_DIRS})
    set(LUAJIT_LIBRARY ${PC_LUAJIT_LINK_LIBRARIES})
    set(LUAJIT_IS_STATIC OFF)
elseif(HAVE_LUAJIT)
    set(LUAJIT_IS_STATIC ${LUAJIT_STATIC})
else()
    message(WARNING "LuaJIT 未找到：Lua 脚本窗口与 Lua IPC 方法将被禁用。")
endif()

if(HAVE_LUAJIT)
    list(APPEND GUI_SOURCES gui/LuaScriptWindow.cpp gui/LuaImGuiWindow.cpp)
    list(APPEND LUA_SOURCES
        lua/LuaEngine.cpp
        lua/LuaAPI.cpp
        lua/LuaAPI_Memory.cpp
        lua/LuaAPI_ImGui.cpp
        lua/LuaAPI_Assembly.cpp
    )
endif()

# Socket 协议层：精简版无搜索/冻结命令
set(SOCKET_SOURCES
    socket/client_singleton.cpp
    socket/ProcessCommands.cpp
    socket/MemoryCommands.cpp
    socket/BreakpointCommands.cpp
    socket/SymbolCommands.cpp
)

set(RENDERER_SOURCES
    renderer/StyleSetup.cpp
)

set(IPC_SOURCES
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
elseif(USE_OPENGL3)
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
    set(PLATFORM_LIBS OpenGL::GL)
    if(glfw3_FOUND)
        list(APPEND PLATFORM_LIBS glfw)
    else()
        list(APPEND PLATFORM_LIBS PkgConfig::GLFW3)
    endif()
else()
    message(FATAL_ERROR "必须启用一个窗口/渲染后端：USE_DX12 或 USE_OPENGL3。")
endif()

add_definitions(-DIMGUI_DISABLE_DEBUG_TOOLS)

if(MSVC)
    add_compile_options(/utf-8)
endif()

add_executable(MiniMemClient
    main.cpp
    ${IMGUI_SOURCES}
    ${GUI_SOURCES}
    ${SOCKET_SOURCES}
    ${RENDERER_SOURCES}
    ${IPC_SOURCES}
    ${LUA_SOURCES}
)

# 包含目录
target_include_directories(MiniMemClient PRIVATE
    imgui imgui/backends gui lua renderer utils ipc
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_BINARY_DIR}/generated
)

if(HAVE_CAPSTONE)
    target_include_directories(MiniMemClient PRIVATE ${CAPSTONE_INCLUDE_DIRS})
endif()
if(HAVE_KEYSTONE)
    target_include_directories(MiniMemClient PRIVATE ${KEYSTONE_INCLUDE_DIRS})
endif()
if(HAVE_LUAJIT)
    target_include_directories(MiniMemClient PRIVATE ${LUAJIT_INCLUDE})
endif()

# 链接系统库
target_link_libraries(MiniMemClient ${PLATFORM_LIBS})

# 链接 LuaJIT
if(HAVE_LUAJIT)
    target_link_libraries(MiniMemClient ${LUAJIT_LIBRARY})
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
    target_link_libraries(MiniMemClient ${CAPSTONE_LIBRARIES})
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
    target_link_libraries(MiniMemClient ${KEYSTONE_LIBRARIES})
    target_compile_definitions(MiniMemClient PRIVATE HAVE_KEYSTONE)
    get_filename_component(_keystone_lib_dir ${KEYSTONE_LIBRARY} DIRECTORY)
    get_filename_component(_keystone_bin_dir "${_keystone_lib_dir}/../bin" ABSOLUTE)
    if(EXISTS "${_keystone_bin_dir}/keystone.dll")
        add_custom_command(TARGET MiniMemClient POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_keystone_bin_dir}/keystone.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/keystone.dll")
    endif()
endif()
