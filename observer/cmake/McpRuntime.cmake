function(shin_install_mcp_runtime)
    if(NOT SHIN_OBSERVER_MCP)
        return()
    endif()
    file(STRINGS "${SHIN_OBSERVER_ROOT}/../VERSION" SHIN_MCP_CORE_VERSION LIMIT_COUNT 1)
    configure_file("${SHIN_OBSERVER_ROOT}/cmake/McpRuntime.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/McpRuntime.cmake" @ONLY)
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/McpRuntime.cmake")
endfunction()
