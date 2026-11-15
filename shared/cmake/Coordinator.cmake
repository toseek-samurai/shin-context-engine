include(FetchContent)
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
FetchContent_Declare(shin_libuv
    URL https://codeload.github.com/libuv/libuv/tar.gz/refs/tags/v1.53.0
    URL_HASH SHA256=279f3f67a24bb9921fe999ca6cd5e332fade8d515873ef9ba054b70e70a31d9e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR shin-no-cmake)
FetchContent_MakeAvailable(shin_libuv)
# Link the static runtime; do not install libuv's development SDK in the kit.
add_subdirectory("${shin_libuv_SOURCE_DIR}" "${shin_libuv_BINARY_DIR}" EXCLUDE_FROM_ALL)
if(MSVC)
    target_compile_options(uv_a PRIVATE "/pathmap:${shin_libuv_SOURCE_DIR}=libuv")
else()
    target_compile_options(uv_a PRIVATE "-ffile-prefix-map=${shin_libuv_SOURCE_DIR}=libuv"
        "-fmacro-prefix-map=${shin_libuv_SOURCE_DIR}=libuv")
endif()
install(FILES "${shin_libuv_SOURCE_DIR}/LICENSE" "${shin_libuv_SOURCE_DIR}/LICENSE-extra"
    DESTINATION share/shin-observer/licenses/libuv)
