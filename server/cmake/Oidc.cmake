include(FetchContent)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(CURL 7.85 REQUIRED)
set(JWT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JWT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JWT_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(JWT_DISABLE_PICOJSON ON CACHE BOOL "" FORCE)
FetchContent_Declare(shin_jwt
    URL https://github.com/Thalhammer/jwt-cpp/releases/download/v0.7.2/jwt-cpp-v0.7.2.tar.gz
    URL_HASH SHA256=7c1b4f767bdd14f727f6114d9e35b761f10d00d6a27ef7650ee8c628f08df27c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(shin_jwt)
set_property(DIRECTORY "${shin_jwt_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)

add_library(shin-server-oidc STATIC src/oidc.cpp src/oidc_provider.cpp)
target_include_directories(shin-server-oidc PUBLIC include)
target_include_directories(shin-server-oidc SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
target_include_directories(shin-server-oidc SYSTEM PRIVATE "${shin_jwt_SOURCE_DIR}/include")
target_link_libraries(shin-server-oidc PRIVATE jwt-cpp::jwt-cpp shin-server-foundation CURL::libcurl)
target_compile_options(shin-server-oidc PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
if(SHIN_SERVER_SANITIZERS)
    target_compile_options(shin-server-oidc PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(shin-server-oidc PUBLIC -fsanitize=address,undefined)
endif()
