include(FetchContent)
FetchContent_Declare(shin_mcp
    URL https://codeload.github.com/jesspig/modelcontextprotocol-cpp-sdk/tar.gz/2304f1bb43e37034f5abf51fd9bd44f555966942
    URL_HASH SHA256=66370718e998119f8bc17c1ac00c8e67d3e1b36ca8d88b84ef3d5aecbe468553
    SOURCE_SUBDIR shin-unused-upstream-build
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(shin_mcp)
find_package(Threads REQUIRED)
# Compile the pinned protocol/server subset with our normal toolchain, without
# upstream auto-LTO, native-CPU flags, HTTP/client transports or examples.
add_library(shin-mcp-sdk STATIC
    ${shin_mcp_SOURCE_DIR}/src/core/JsonValue.cpp
    ${shin_mcp_SOURCE_DIR}/src/core/JsonParser.cpp
    ${shin_mcp_SOURCE_DIR}/src/core/JsonRpc.cpp
    ${shin_mcp_SOURCE_DIR}/src/core/McpTypesTools.cpp
    ${shin_mcp_SOURCE_DIR}/src/core/McpTypesResults.cpp
    ${shin_mcp_SOURCE_DIR}/src/core/McpTypesNotifications.cpp
    ${shin_mcp_SOURCE_DIR}/src/core/McpTypesTasks.cpp
    ${shin_mcp_SOURCE_DIR}/src/core/McpTypesParams.cpp
    ${shin_mcp_SOURCE_DIR}/src/core/Content.cpp
    ${shin_mcp_SOURCE_DIR}/src/core/Capabilities.cpp
    ${shin_mcp_SOURCE_DIR}/src/detail/AtomicJsonFile.cpp
    ${shin_mcp_SOURCE_DIR}/src/protocol/WireCodec.cpp
    ${shin_mcp_SOURCE_DIR}/src/protocol/McpSessionHandler.cpp
    ${shin_mcp_SOURCE_DIR}/src/server/McpServer.cpp
    ${shin_mcp_SOURCE_DIR}/src/server/FileTaskStore.cpp)
target_include_directories(shin-mcp-sdk SYSTEM PUBLIC ${shin_mcp_SOURCE_DIR}/include)
target_include_directories(shin-mcp-sdk PRIVATE ${shin_mcp_SOURCE_DIR}/src)
# The SDK reports source locations through __FILE__. Keep those diagnostics
# useful while preventing a machine-specific FetchContent path from entering
# packaged binaries.
if(CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
    target_compile_options(shin-mcp-sdk PRIVATE
        "-fmacro-prefix-map=${shin_mcp_SOURCE_DIR}=shin-mcp-sdk")
endif()
target_link_libraries(shin-mcp-sdk PUBLIC Threads::Threads)
if(WIN32)
    target_compile_definitions(shin-mcp-sdk PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
endif()
