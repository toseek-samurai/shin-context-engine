include(FetchContent)
function(shin_configure_ftxui)
    # Ship one executable, without upstream tests, examples or runtime libraries.
    set(BUILD_SHARED_LIBS OFF)
    foreach(option BUILD_DOCS BUILD_EXAMPLES BUILD_MODULES BUILD_TESTS BUILD_TESTS_FUZZER
            CLANG_TIDY DEV_WARNINGS ENABLE_COVERAGE ENABLE_INSTALL ENABLE_CCACHE)
        set(FTXUI_${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(FTXUI_QUIET ON CACHE BOOL "" FORCE)
    FetchContent_Declare(shin_ftxui
        URL https://codeload.github.com/ArthurSonzogni/FTXUI/tar.gz/refs/tags/v7.0.3
        URL_HASH SHA256=e7c62ffe19009759821b4f0f8df7f2a6fb83784c3a9f1477d81f56d3ee723c88
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(shin_ftxui)
    foreach(library screen dom component)
        target_include_directories(${library} SYSTEM INTERFACE "${shin_ftxui_SOURCE_DIR}/include")
        if(CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
            target_compile_options(${library} PRIVATE "-fmacro-prefix-map=${shin_ftxui_SOURCE_DIR}=ftxui")
        endif()
    endforeach()
    install(FILES "${shin_ftxui_SOURCE_DIR}/LICENSE" DESTINATION share/shin-observer/licenses/ftxui)
endfunction()
shin_configure_ftxui()
