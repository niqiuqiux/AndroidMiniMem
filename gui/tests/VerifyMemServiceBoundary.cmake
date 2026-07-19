if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(GLOB_RECURSE frontend_sources LIST_DIRECTORIES false
    "${SOURCE_ROOT}/gui/*.h"
    "${SOURCE_ROOT}/gui/*.cpp"
    "${SOURCE_ROOT}/lua/*.h"
    "${SOURCE_ROOT}/lua/*.cpp"
    "${SOURCE_ROOT}/ipc/*.h"
    "${SOURCE_ROOT}/ipc/*.cpp")
list(APPEND frontend_sources "${SOURCE_ROOT}/main.cpp")

set(protocol_markers
    "client_singleton\\.h"
    "SocketCommand::"
    "GetSocketMgr\\("
    "PORT_(MAIN|DEBUG|ERROR)"
    "FetchServerVersion\\("
    "GetMemType\\("
    "InitDriver\\("
    "FetchProcessList\\("
    "OpenProcessHandle\\("
    "CloseProcessHandle\\("
    "FetchModuleList\\("
    "ReadProcessMemoryBytes\\("
    "ReadBratchAddr\\("
    "WriteProcessMemoryBytes\\("
    "ResolveModuleOffsetChain\\("
    "SetKernelBreakpoint\\("
    "RemoveKernelBreakpoint\\("
    "SuspendKernelBreakpoint\\("
    "ResumeKernelBreakpoint\\("
    "ReadKernelBreakpointInfo"
    "Symbol(Init|GetList|Find)\\(")

foreach(source IN LISTS frontend_sources)
    file(READ "${source}" content)
    foreach(marker IN LISTS protocol_markers)
        if(content MATCHES "${marker}")
            file(RELATIVE_PATH relative "${SOURCE_ROOT}" "${source}")
            message(FATAL_ERROR
                "front-end MemService boundary violation in ${relative}: ${marker}")
        endif()
    endforeach()
endforeach()

file(READ "${SOURCE_ROOT}/lua/LuaEngine.cpp" lua_engine_source)
foreach(required IN ITEMS
        "LuaOperationBinding"
        "LuaAPI::BindOperationContext"
        "context_.deadline = deadline")
    string(FIND "${lua_engine_source}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "Lua execution context binding is missing: ${required}")
    endif()
endforeach()
foreach(lua_adapter IN ITEMS LuaAPI_Memory.cpp LuaAPI_Assembly.cpp)
    file(READ "${SOURCE_ROOT}/lua/${lua_adapter}" lua_adapter_source)
    if(lua_adapter_source MATCHES "service\\.captureContext\\(")
        message(FATAL_ERROR
            "Lua adapter bypasses its bound operation context: ${lua_adapter}")
    endif()
endforeach()

file(GLOB_RECURSE state_sources LIST_DIRECTORIES false
    "${SOURCE_ROOT}/gui/*.h"
    "${SOURCE_ROOT}/gui/*.cpp"
    "${SOURCE_ROOT}/socket/*.h"
    "${SOURCE_ROOT}/socket/*.cpp"
    "${SOURCE_ROOT}/mem/*.h"
    "${SOURCE_ROOT}/mem/*.cpp")
foreach(source IN LISTS state_sources)
    string(REPLACE "\\" "/" normalized "${source}")
    if(normalized MATCHES "/gui/AppContext\\.(h|cpp)$")
        continue()
    endif()
    file(READ "${source}" content)
    if(content MATCHES "(selectedPid|processHandle)\\.store\\(")
        file(RELATIVE_PATH relative "${SOURCE_ROOT}" "${source}")
        message(FATAL_ERROR
            "target state may only be published by AppContext: ${relative}")
    endif()
endforeach()

file(READ "${SOURCE_ROOT}/mem/SystemMemService.cpp" backend_source)
foreach(required IN ITEMS
        "AcquireRequestLease()"
        "beginTargetMutation("
        "SocketCommand::TransactionLease transaction(PORT_MAIN)"
        "mutation->publish(pid, handle, name)")
    string(FIND "${backend_source}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "transactional process switching contract is missing: ${required}")
    endif()
endforeach()

file(READ "${SOURCE_ROOT}/socket/client.hpp" client_source)
if(client_source MATCHES "DrainPending")
    message(FATAL_ERROR "DrainPending must not return to the unframed protocol")
endif()
foreach(required IN ITEMS
        "PoisonAndClose()"
        "SetPoisonCallback"
        "InvalidateProtocol()")
    string(FIND "${client_source}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "socket poisoning contract is missing: ${required}")
    endif()
endforeach()

file(GLOB_RECURSE lean_product_sources LIST_DIRECTORIES false
    "${SOURCE_ROOT}/mem/*.h"
    "${SOURCE_ROOT}/mem/*.cpp"
    "${SOURCE_ROOT}/socket/*.h"
    "${SOURCE_ROOT}/socket/*.cpp"
    "${SOURCE_ROOT}/gui/*.h"
    "${SOURCE_ROOT}/gui/*.cpp"
    "${SOURCE_ROOT}/ipc/*.h"
    "${SOURCE_ROOT}/ipc/*.cpp"
    "${SOURCE_ROOT}/lua/*.h"
    "${SOURCE_ROOT}/lua/*.cpp")
foreach(source IN LISTS lean_product_sources)
    file(READ "${source}" content)
    foreach(forbidden IN ITEMS
            "SCAN_TYPE"
            "ScanResult"
            "FreezeManager"
            "InjectSo"
            "PointerScan"
            "Point_Scan")
        string(FIND "${content}" "${forbidden}" position)
        if(NOT position EQUAL -1)
            file(RELATIVE_PATH relative "${SOURCE_ROOT}" "${source}")
            message(FATAL_ERROR
                "removed product capability leaked into ${relative}: ${forbidden}")
        endif()
    endforeach()
endforeach()

file(READ "${SOURCE_ROOT}/ipc/IpcServer.cpp" ipc_server_source)
file(READ "${SOURCE_ROOT}/ipc/IpcHttpRequest.cpp" ipc_http_source)
set(ipc_source "${ipc_server_source}\n${ipc_http_source}")
foreach(forbidden IN ITEMS
        "Access-Control-Allow-Origin"
        "requestLine.method == \"OPTIONS\"")
    string(FIND "${ipc_source}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR
            "browser-accessible IPC behavior must not return: ${forbidden}")
    endif()
endforeach()
foreach(required IN ITEMS
        "IpcHttp::ValidateRequestHead(headers)"
        "requestLine.method != \"POST\""
        "isJsonContentType(contentType->second)"
        "parsedHeaders.find(\"origin\")"
        "parsedHeaders.find(\"transfer-encoding\")")
    string(FIND "${ipc_source}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "IPC HTTP boundary check is missing: ${required}")
    endif()
endforeach()

file(READ "${SOURCE_ROOT}/gui/ServerConnectWindow.cpp" connect_window_source)
file(READ "${SOURCE_ROOT}/gui/ServerConnectWindow.h" connect_window_header)
set(connect_window_content
    "${connect_window_source}\n${connect_window_header}")
foreach(forbidden IN ITEMS
        "cardKeyBuf[256] = \""
        "getString(\"cardKey\""
        "setString(\"cardKey\""
        "cardKey.c_str()")
    string(FIND "${connect_window_content}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR
            "driver card secret handling regressed: ${forbidden}")
    endif()
endforeach()
foreach(required IN ITEMS
        "ImGuiInputTextFlags_Password"
        "config.remove(\"cardKey\")")
    string(FIND "${connect_window_content}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "driver card secret protection is missing: ${required}")
    endif()
endforeach()

get_filename_component(project_root "${SOURCE_ROOT}" DIRECTORY)
file(READ "${project_root}/engine/ceserver/api.cpp" engine_api_source)
foreach(forbidden IN ITEMS
        "usedParams.c_str()"
        "LOGDF(\"InitReadWriteDriver: card")
    string(FIND "${engine_api_source}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR
            "driver authorization data must not be written to engine logs: ${forbidden}")
    endif()
endforeach()

file(READ "${project_root}/engine/android/AndroidTracer.hpp" tracer_source)
foreach(forbidden IN ITEMS
        "InjectSo"
        "CallRmmap"
        "CallRmunmap"
        "set_selinux_state"
        "handle_selinux_init")
    string(FIND "${tracer_source}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR
            "removed injection capability leaked into AndroidTracer: ${forbidden}")
    endif()
endforeach()

message(STATUS "Verified GUI/Lua/IPC -> IMemService boundary")
