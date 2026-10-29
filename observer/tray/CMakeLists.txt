if(APPLE)
    enable_language(OBJCXX)
endif()
include(FetchContent)
FetchContent_Declare(shin_imgui
    URL https://codeload.github.com/ocornut/imgui/tar.gz/refs/tags/v1.93.0
    URL_HASH SHA256=f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee
    # Rebuild changed headers and sources even when archive timestamps are older.
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(shin_imgui)
if(WIN32)
    include(windows.cmake)
    return()
endif()
add_library(shin-imgui STATIC
    ${shin_imgui_SOURCE_DIR}/imgui.cpp ${shin_imgui_SOURCE_DIR}/imgui_draw.cpp
    ${shin_imgui_SOURCE_DIR}/imgui_tables.cpp ${shin_imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${shin_imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm ${shin_imgui_SOURCE_DIR}/backends/imgui_impl_osx.mm)
target_include_directories(shin-imgui SYSTEM PUBLIC ${shin_imgui_SOURCE_DIR} ${shin_imgui_SOURCE_DIR}/backends)
target_compile_options(shin-imgui PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
target_link_libraries(shin-imgui PUBLIC "-framework Cocoa" "-framework Metal" "-framework MetalKit" "-framework QuartzCore" "-framework GameController")
add_executable(shin-tray MACOSX_BUNDLE main.mm ../ui/update_window_macos.mm)
target_compile_definitions(shin-tray PRIVATE IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_sources(shin-tray PRIVATE assets/ShinTrayTemplate.png)
set_source_files_properties(assets/ShinTrayTemplate.png PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set(SHIN_MONITOR_LOGO "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/images/shin-logo.png")
target_sources(shin-tray PRIVATE "${SHIN_MONITOR_LOGO}")
set_source_files_properties("${SHIN_MONITOR_LOGO}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
target_link_libraries(shin-tray PRIVATE "-framework ImageIO")
target_sources(shin-tray PRIVATE ${shin_imgui_SOURCE_DIR}/LICENSE.txt)
set_source_files_properties(${shin_imgui_SOURCE_DIR}/LICENSE.txt PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licenses)
target_compile_options(shin-tray PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
target_link_libraries(shin-tray PRIVATE shin-observer-native shin-imgui "-framework ServiceManagement")
if(BUILD_TESTING)
    enable_testing()
    add_executable(shin-tray-diagnostics-tests project_diagnostics_test.cpp)
    target_compile_options(shin-tray-diagnostics-tests PRIVATE -UNDEBUG)
    target_link_libraries(shin-tray-diagnostics-tests PRIVATE shin-observer-native)
    add_test(NAME shin-tray-diagnostics COMMAND shin-tray-diagnostics-tests)
    add_executable(shin-tray-request-timing-tests request_timing_test.cpp)
    target_compile_options(shin-tray-request-timing-tests PRIVATE -UNDEBUG)
    target_link_libraries(shin-tray-request-timing-tests PRIVATE shin-observer-native)
    add_test(NAME shin-tray-request-timing COMMAND shin-tray-request-timing-tests)
    add_executable(shin-tray-layout-tests main.mm)
    target_compile_definitions(shin-tray-layout-tests PRIVATE SHIN_TRAY_LAYOUT_TEST=1 IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_compile_definitions(shin-tray-layout-tests PRIVATE SHIN_TRAY_TEST_LOGO="${SHIN_MONITOR_LOGO}")
    target_compile_options(shin-tray-layout-tests PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
    target_link_libraries(shin-tray-layout-tests PRIVATE shin-observer-native shin-imgui "-framework ImageIO" "-framework ServiceManagement")
    add_test(NAME shin-tray-layout COMMAND shin-tray-layout-tests "${CMAKE_CURRENT_BINARY_DIR}/layout-check"
        -ShinHome /shin-monitor-test-home)
    set_tests_properties(shin-tray-layout PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RESOURCE_LOCK shin-tray-render)
    add_test(NAME shin-tray-layout-en COMMAND shin-tray-layout-tests "${CMAKE_CURRENT_BINARY_DIR}/layout-check-en"
        -ShinHome /shin-monitor-test-home en)
    set_tests_properties(shin-tray-layout-en PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RESOURCE_LOCK shin-tray-render)
    add_executable(shin-tray-update-window-tests MACOSX_BUNDLE main.mm)
    set_target_properties(shin-tray-update-window-tests PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/test_info.plist.in")
    target_compile_definitions(shin-tray-update-window-tests PRIVATE SHIN_TRAY_UPDATE_WINDOW_TEST=1 IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_compile_options(shin-tray-update-window-tests PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
    target_link_libraries(shin-tray-update-window-tests PRIVATE shin-observer-native shin-imgui "-framework ImageIO" "-framework ServiceManagement")
    add_test(NAME shin-tray-cli-handoff COMMAND ${Python3_EXECUTABLE} -B
        "${CMAKE_CURRENT_SOURCE_DIR}/cli_handoff_test.py" $<TARGET_FILE:shin-tray-update-window-tests>)
    set_tests_properties(shin-tray-cli-handoff PROPERTIES TIMEOUT 45 RESOURCE_LOCK shin-tray-render)
    foreach(language IN ITEMS en ru)
        add_test(NAME shin-tray-update-window-${language} COMMAND shin-tray-update-window-tests
            "${CMAKE_CURRENT_BINARY_DIR}/update-window-${language}" ${language})
        set_tests_properties(shin-tray-update-window-${language} PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77 RESOURCE_LOCK shin-tray-render)
    endforeach()
    add_test(NAME shin-update-progress-transport COMMAND ${Python3_EXECUTABLE} -B
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/update_progress_transport_test.py" $<TARGET_FILE:shin-tray>)
    set_tests_properties(shin-update-progress-transport PROPERTIES TIMEOUT 45 RESOURCE_LOCK shin-tray-render)
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" SHIN_MONITOR_SHORT_VERSION "${SHIN_OBSERVER_VERSION}")
set_target_properties(shin-tray PROPERTIES
    OUTPUT_NAME "Shin Monitor"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
find_program(SHIN_CODESIGN codesign REQUIRED)
add_custom_command(TARGET shin-tray POST_BUILD
    COMMAND "${SHIN_CODESIGN}" --force --sign - "$<TARGET_BUNDLE_DIR:shin-tray>"
    VERBATIM)
install(TARGETS shin-tray BUNDLE DESTINATION .)
install(FILES ${shin_imgui_SOURCE_DIR}/LICENSE.txt DESTINATION share/shin-observer/licenses/imgui)
