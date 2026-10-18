# Native parsers keep client settings independent of Python, Node and agent CLIs.
include(FetchContent)
FetchContent_Declare(shin_toml
    URL https://codeload.github.com/marzer/tomlplusplus/tar.gz/refs/tags/v3.4.0
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR shin-no-cmake)
FetchContent_Declare(shin_yaml
    URL https://codeload.github.com/yaml/libyaml/tar.gz/refs/tags/0.2.5
    URL_HASH SHA256=fa240dbf262be053f3898006d502d514936c818e422afdcf33921c63bed9bf2e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR shin-no-cmake)
FetchContent_MakeAvailable(shin_toml shin_yaml)
add_library(shin-client-yaml STATIC
    ${shin_yaml_SOURCE_DIR}/src/api.c ${shin_yaml_SOURCE_DIR}/src/dumper.c
    ${shin_yaml_SOURCE_DIR}/src/emitter.c ${shin_yaml_SOURCE_DIR}/src/loader.c
    ${shin_yaml_SOURCE_DIR}/src/parser.c ${shin_yaml_SOURCE_DIR}/src/reader.c
    ${shin_yaml_SOURCE_DIR}/src/scanner.c ${shin_yaml_SOURCE_DIR}/src/writer.c)
target_include_directories(shin-client-yaml SYSTEM PUBLIC ${shin_yaml_SOURCE_DIR}/include)
target_compile_definitions(shin-client-yaml PUBLIC YAML_DECLARE_STATIC PRIVATE
    YAML_VERSION_MAJOR=0 YAML_VERSION_MINOR=2 YAML_VERSION_PATCH=5 YAML_VERSION_STRING="0.2.5")
target_include_directories(shin-observer-native SYSTEM PRIVATE ${shin_toml_SOURCE_DIR}/include)
target_link_libraries(shin-observer-native PRIVATE shin-client-yaml)
install(FILES ${shin_toml_SOURCE_DIR}/LICENSE DESTINATION share/shin-observer/licenses/tomlplusplus)
install(FILES ${shin_yaml_SOURCE_DIR}/License DESTINATION share/shin-observer/licenses/libyaml)
