# ============================================================================
# Socket 接口构建配置（MiniMem）
# ============================================================================

# ----------------------------------------------------------------------------
# Socket Server 可执行文件（设备端常驻服务，供 Windows GUI / MCP 连接）
# ----------------------------------------------------------------------------
add_executable(mini_server
    server.cpp
)

target_link_libraries(mini_server PRIVATE
    server_core
    common_interface
)

apply_obfuscation(mini_server)
apply_strip(mini_server)
