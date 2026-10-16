set(SHIN_CORE_ALLOCATOR "system" CACHE STRING "Standalone Core CLI allocator: system or mimalloc")
set_property(CACHE SHIN_CORE_ALLOCATOR PROPERTY STRINGS system mimalloc)
if(NOT SHIN_CORE_ALLOCATOR STREQUAL "system" AND NOT SHIN_CORE_ALLOCATOR STREQUAL "mimalloc")
    message(FATAL_ERROR "SHIN_CORE_ALLOCATOR must be system or mimalloc")
endif()
if(SHIN_CORE_ALLOCATOR STREQUAL "system")
    return()
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT SHIN_CONTEXT_BUILD_CLI OR SHIN_ENABLE_SANITIZERS)
    message(FATAL_ERROR "mimalloc is opt-in for the non-sanitized standalone Linux Core CLI only")
endif()
if(CMAKE_VERSION VERSION_LESS "3.24")
    message(FATAL_ERROR "The optional mimalloc build requires CMake 3.24 or newer")
endif()

include(FetchContent)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT ON CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE ON CACHE BOOL "" FORCE)
set(MI_ALLOW_THP OFF CACHE STRING "" FORCE)
set(MI_OPT_ARCH OFF CACHE STRING "" FORCE)
FetchContent_Declare(shin_mimalloc
    URL https://codeload.github.com/microsoft/mimalloc/tar.gz/refs/tags/v3.5.3
    URL_HASH SHA256=3b4a15153a59905995f7070296ed604bb5ccc00cabb8b93446931aff77224d47
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(shin_mimalloc)
set_property(DIRECTORY "${shin_mimalloc_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
target_compile_options(mimalloc-obj PRIVATE "-ffile-prefix-map=${shin_mimalloc_SOURCE_DIR}=mimalloc")

function(shin_link_core_allocator target)
    # The object supplies overrides; the static target supplies platform link dependencies.
    target_link_libraries(${target} PRIVATE mimalloc-obj mimalloc-static)
endfunction()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/shin-core-allocator.json"
    "{\"schema_version\":\"shin.core-allocator.v1\",\"id\":\"mimalloc\",\"version\":\"3.5.3\",\"scope\":\"standalone-linux-core-cli\",\"linkage\":\"static-object\",\"transparent_huge_pages\":false,\"source_sha256\":\"3b4a15153a59905995f7070296ed604bb5ccc00cabb8b93446931aff77224d47\"}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/shin-core-allocator.json"
    DESTINATION share/shin-context)
install(FILES "${shin_mimalloc_SOURCE_DIR}/LICENSE"
    DESTINATION share/shin-context/licenses/mimalloc)
