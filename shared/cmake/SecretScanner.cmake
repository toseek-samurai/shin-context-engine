include(FetchContent)

function(shin_configure_secret_scanner)
    # Static, pinned upstreams; do not add their tests, tools or install payloads.
    set(BUILD_SHARED_LIBS OFF)
    set(ABSL_PROPAGATE_CXX_STD ON)
    set(ABSL_BUILD_TESTING OFF)
    set(ABSL_ENABLE_INSTALL OFF)
    set(RE2_BUILD_TESTING OFF)
    set(RE2_TEST OFF)
    set(RE2_BENCHMARK OFF)
    set(RE2_USE_ICU OFF)
    set(RE2_INSTALL OFF)
    FetchContent_Declare(shin_abseil
        URL https://codeload.github.com/abseil/abseil-cpp/tar.gz/refs/tags/20250814.1
        URL_HASH SHA256=1692f77d1739bacf3f94337188b78583cf09bab7e420d2dc6c5605a4f86785a1
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR shin-no-cmake)
    FetchContent_Declare(shin_re2
        URL https://codeload.github.com/google/re2/tar.gz/refs/tags/2025-11-05
        URL_HASH SHA256=87f6029d2f6de8aa023654240a03ada90e876ce9a4676e258dd01ea4c26ffd67
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR shin-no-cmake)
    FetchContent_MakeAvailable(shin_abseil shin_re2)
    get_directory_property(saved_compile_options COMPILE_OPTIONS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
        add_compile_options("-fmacro-prefix-map=${shin_re2_SOURCE_DIR}=re2"
            "-fmacro-prefix-map=${shin_abseil_SOURCE_DIR}=abseil")
    endif()
    add_subdirectory("${shin_abseil_SOURCE_DIR}" "${shin_abseil_BINARY_DIR}" EXCLUDE_FROM_ALL)
    add_subdirectory("${shin_re2_SOURCE_DIR}" "${shin_re2_BINARY_DIR}" EXCLUDE_FROM_ALL)
    set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "${saved_compile_options}")
    target_include_directories(shin-context-core SYSTEM PRIVATE
        "${shin_re2_SOURCE_DIR}" "${shin_abseil_SOURCE_DIR}")
    set_property(TARGET re2 PROPERTY SHIN_RE2_LICENSE "${shin_re2_SOURCE_DIR}/LICENSE")
    set_property(TARGET re2 PROPERTY SHIN_ABSEIL_LICENSE "${shin_abseil_SOURCE_DIR}/LICENSE")
endfunction()

shin_configure_secret_scanner()

function(shin_install_secret_scanner_notices component)
    get_target_property(re2_license re2 SHIN_RE2_LICENSE)
    get_target_property(abseil_license re2 SHIN_ABSEIL_LICENSE)
    install(FILES "${re2_license}" DESTINATION "share/${component}/licenses/re2")
    install(FILES "${abseil_license}" DESTINATION "share/${component}/licenses/abseil")
endfunction()
