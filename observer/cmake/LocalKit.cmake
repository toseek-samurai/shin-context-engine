# Keep the matched kit separate from the component-only install contract.
get_filename_component(crypto_library "${OPENSSL_CRYPTO_LIBRARY}" REALPATH)
get_filename_component(crypto_directory "${crypto_library}" DIRECTORY)
find_file(SHIN_OPENSSL_LICENSE NAMES LICENSE.txt LICENSE
    HINTS "${crypto_directory}/.." "${OPENSSL_INCLUDE_DIR}/.." "${OPENSSL_ROOT_DIR}"
    REQUIRED)
install(FILES "${SHIN_OPENSSL_LICENSE}" DESTINATION share/shin-observer/licenses/openssl)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/local-kit.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/../docs/native-client-setup.md"
    DESTINATION share/shin-observer)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../integrations/mcp/skills/shin-context"
    DESTINATION share/shin-context/mcp/skills)
configure_file("${CMAKE_CURRENT_LIST_DIR}/FixupLocalKit.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/FixupLocalKit.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/FixupLocalKit.cmake")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" SHIN_KIT_CORE_VERSION LIMIT_COUNT 1)
configure_file("${CMAKE_CURRENT_LIST_DIR}/ManifestLocalKit.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ManifestLocalKit.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/ManifestLocalKit.cmake")

# CPack's ZIP writer omits AppleDouble filesystem metadata. Its TGZ writer on
# macOS can add ._* resources which invalidate an app after generic extraction.
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "shin-local")
set(CPACK_PACKAGE_VENDOR "Shin Context Engine")
set(CPACK_PACKAGE_VERSION "${SHIN_OBSERVER_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "shin-local-${SHIN_OBSERVER_VERSION}-macos-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packages")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_CHECKSUM SHA256)
include(CPack)
