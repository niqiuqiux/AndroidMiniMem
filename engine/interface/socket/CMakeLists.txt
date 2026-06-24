# ============================================================================
# Socket 接口构建配置（MiniMem）
# ============================================================================

# ----------------------------------------------------------------------------
# Socket Server 可执行文件（设备端常驻服务，供 Windows GUI / MCP 连接）
# ----------------------------------------------------------------------------
add_executable(socket_server
    server.cpp
)

target_link_libraries(socket_server PRIVATE
    server_core
    common_interface
)

apply_obfuscation(socket_server)
apply_strip(socket_server)
