cmake_minimum_required(VERSION 3.24)
project(ShinObserver LANGUAGES C CXX)
include("${CMAKE_CURRENT_SOURCE_DIR}/../shared/cmake/WindowsBuild.cmake")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(SHIN_OBSERVER_SANITIZERS "Enable ASan/UBSan" OFF)
option(SHIN_OBSERVER_TSAN "Enable TSan" OFF)
option(SHIN_OBSERVER_SOURCE_DISCOVERY "Enable local source queries with a private SQLite cache" ON)
option(SHIN_OBSERVER_MCP "Build the native project-bound MCP stdio adapter" OFF)
option(SHIN_OBSERVER_TRAY "Build the native desktop ImGui tray client" OFF)
option(SHIN_OBSERVER_LOCAL_KIT "Package the complete relocatable macOS local kit" OFF)
option(SHIN_OBSERVER_WINDOWS_KIT "Package the native Windows desktop candidate (ZIP/MSI)" OFF)
if(SHIN_OBSERVER_WINDOWS_KIT AND (NOT WIN32 OR NOT SHIN_OBSERVER_SOURCE_DISCOVERY OR NOT SHIN_OBSERVER_MCP OR NOT SHIN_OBSERVER_TRAY))
    message(FATAL_ERROR "Windows kit requires native Windows, source discovery, MCP and Monitor")
endif()
if(SHIN_OBSERVER_LOCAL_KIT AND (NOT APPLE OR NOT SHIN_OBSERVER_SOURCE_DISCOVERY OR NOT SHIN_OBSERVER_MCP OR NOT SHIN_OBSERVER_TRAY))
    message(FATAL_ERROR "The qualified local kit requires macOS, source discovery, MCP and Monitor")
endif()
if(SHIN_OBSERVER_LOCAL_KIT AND (SHIN_OBSERVER_SANITIZERS OR SHIN_OBSERVER_TSAN))
    message(FATAL_ERROR "Sanitizer runtimes are not portable release kit dependencies")
endif()
if(SHIN_OBSERVER_TRAY AND ((NOT APPLE AND NOT WIN32) OR NOT SHIN_OBSERVER_SOURCE_DISCOVERY))
    message(FATAL_ERROR "Monitor requires macOS/Windows and source discovery")
endif()
if(SHIN_OBSERVER_MCP AND NOT SHIN_OBSERVER_SOURCE_DISCOVERY)
    message(FATAL_ERROR "Native MCP requires SHIN_OBSERVER_SOURCE_DISCOVERY")
endif()
if(SHIN_OBSERVER_SANITIZERS AND SHIN_OBSERVER_TSAN)
    message(FATAL_ERROR "Choose either ASan/UBSan or TSan")
endif()
if(SHIN_OBSERVER_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
if(SHIN_OBSERVER_LOCAL_KIT OR SHIN_OBSERVER_WINDOWS_KIT)
    set(SHIN_CONTEXT_BUILD_CLI ON CACHE BOOL "" FORCE)
else()
    set(SHIN_CONTEXT_BUILD_CLI OFF CACHE BOOL "" FORCE)
endif()
set(SHIN_ENABLE_SANITIZERS ${SHIN_OBSERVER_SANITIZERS} CACHE BOOL "" FORCE)
set(SHIN_CONTEXT_BUILD_SOURCE_DISCOVERY ${SHIN_OBSERVER_SOURCE_DISCOVERY} CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_BINARY_DIR}/core")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Native.cmake")
if(SHIN_OBSERVER_TRAY)
    add_subdirectory(tray)
    if(APPLE)
        list(APPEND SHIN_LOCAL_ANALYSIS "${CMAKE_CURRENT_SOURCE_DIR}/tray/main.mm")
    else()
        list(APPEND SHIN_LOCAL_ANALYSIS "${CMAKE_CURRENT_SOURCE_DIR}/tray/windows_main.cpp")
    endif()
endif()
find_program(SHIN_OBSERVER_CLANG_TIDY NAMES clang-tidy-23 clang-tidy HINTS /opt/homebrew/opt/llvm/bin)
if(SHIN_OBSERVER_CLANG_TIDY)
    if(BUILD_TESTING AND NOT WIN32)
        list(APPEND SHIN_LOCAL_ANALYSIS
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/io_test.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_view_test.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/io_interleaving_test.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/outbox_test.cpp")
    endif()
    add_custom_target(shin-observer-static-analysis
        COMMAND "${SHIN_OBSERVER_CLANG_TIDY}" -p "${CMAKE_CURRENT_BINARY_DIR}"
            "--header-filter=^.*/observer/(src|include|tests)/"
            ${SHIN_LOCAL_ANALYSIS}
        VERBATIM)
endif()
include(CTest)
if(BUILD_TESTING AND WIN32 AND SHIN_OBSERVER_SOURCE_DISCOVERY)
    add_subdirectory(platform/windows)
endif()
if(BUILD_TESTING AND NOT WIN32)
    if(SHIN_OBSERVER_SOURCE_DISCOVERY)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME shin-preview-publication
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_publish_preview.py)
        set_tests_properties(shin-preview-publication PROPERTIES TIMEOUT 30)
        add_executable(shin-resident-retention-tests tests/resident_retention_test.cpp)
        target_link_libraries(shin-resident-retention-tests PRIVATE shin-observer-native)
        add_test(NAME shin-resident-retention COMMAND shin-resident-retention-tests)
        add_executable(shin-source-coverage-tests tests/source_coverage_test.cpp)
        target_link_libraries(shin-source-coverage-tests PRIVATE shin-observer-native)
        add_test(NAME shin-source-coverage COMMAND shin-source-coverage-tests ${CMAKE_CURRENT_SOURCE_DIR}/../profiles)
        add_executable(shin-local-worker-process-tests tests/local_worker_process_test.cpp)
        target_link_libraries(shin-local-worker-process-tests PRIVATE shin-observer-native)
        target_compile_definitions(shin-local-worker-process-tests PRIVATE SHIN_TEST_PROFILES="${CMAKE_CURRENT_SOURCE_DIR}/../profiles")
        add_test(NAME shin-local-worker-process COMMAND shin-local-worker-process-tests)
        set_tests_properties(shin-local-worker-process PROPERTIES TIMEOUT 90)
        add_test(NAME shin-installed-probe
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_installed_probe.py)
        add_executable(shin-local-changes-tests tests/local_changes_test.cpp)
        target_link_libraries(shin-local-changes-tests PRIVATE shin-observer-native)
        add_test(NAME shin-local-changes COMMAND shin-local-changes-tests)
        set_tests_properties(shin-local-changes PROPERTIES TIMEOUT 30)
        add_executable(shin-model-usage-tests tests/model_usage_test.cpp)
        target_link_libraries(shin-model-usage-tests PRIVATE shin-observer-native)
        if(TARGET SQLite3::SQLite3)
            target_link_libraries(shin-model-usage-tests PRIVATE SQLite3::SQLite3)
        else()
            target_link_libraries(shin-model-usage-tests PRIVATE SQLite::SQLite3)
        endif()
        add_test(NAME shin-model-usage COMMAND shin-model-usage-tests)
        add_test(NAME shin-usage-cli COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/usage_cli_test.py $<TARGET_FILE:shin-usage>)
        set_tests_properties(shin-usage-cli PROPERTIES TIMEOUT 45)
        add_test(NAME shin-usage-registered COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/usage_registered_test.py $<TARGET_FILE:shin-ce>
            $<TARGET_FILE:shin-usage> ${CMAKE_CURRENT_SOURCE_DIR}/../profiles)
        set_tests_properties(shin-usage-registered PROPERTIES TIMEOUT 120)
        add_executable(shin-local-telemetry-tests tests/local_telemetry_test.cpp)
        target_link_libraries(shin-local-telemetry-tests PRIVATE shin-observer-native ${SHIN_NATIVE_SQLITE})
        add_test(NAME shin-local-telemetry COMMAND shin-local-telemetry-tests)
        add_executable(shin-local-status-tests tests/local_status_test.cpp)
        target_link_libraries(shin-local-status-tests PRIVATE shin-observer-native)
        add_test(NAME shin-local-status COMMAND shin-local-status-tests)
        add_executable(shin-local-refresh-race-tests tests/local_refresh_race_test.cpp)
        target_link_libraries(shin-local-refresh-race-tests PRIVATE shin-observer-native)
        add_test(NAME shin-local-refresh-races COMMAND shin-local-refresh-race-tests ${CMAKE_CURRENT_SOURCE_DIR}/../profiles)
        set_tests_properties(shin-local-refresh-races PROPERTIES TIMEOUT 45)
        add_test(NAME shin-ce-local-storage COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_project_test.py $<TARGET_FILE:shin-ce> ${CMAKE_CURRENT_SOURCE_DIR}/../profiles StorageTests)
        set_tests_properties(shin-ce-local-storage PROPERTIES TIMEOUT 120)
        add_test(NAME shin-ce-local-project
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_project_test.py
                $<TARGET_FILE:shin-ce> ${CMAKE_CURRENT_SOURCE_DIR}/../profiles LocalProjectTests)
        set_tests_properties(shin-ce-local-project PROPERTIES TIMEOUT 180)
        add_test(NAME shin-ce-local-identity
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_identity_test.py
                $<TARGET_FILE:shin-ce> ${CMAKE_CURRENT_SOURCE_DIR}/../profiles)
        set_tests_properties(shin-ce-local-identity PROPERTIES TIMEOUT 90)
        add_test(NAME shin-ce-local-watch
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_watch_test.py
                $<TARGET_FILE:shin-ce> ${CMAKE_CURRENT_SOURCE_DIR}/../profiles)
        set_tests_properties(shin-ce-local-watch PROPERTIES TIMEOUT 180)
        add_test(NAME shin-ce-local-multi-watch
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_multi_watch_test.py
                $<TARGET_FILE:shin-ce> ${CMAKE_CURRENT_SOURCE_DIR}/../profiles)
        set_tests_properties(shin-ce-local-multi-watch PROPERTIES TIMEOUT 180)
        add_test(NAME shin-ce-local-service
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_service_test.py
                $<TARGET_FILE:shin-ce> ${CMAKE_CURRENT_SOURCE_DIR}/../profiles)
        set_tests_properties(shin-ce-local-service PROPERTIES TIMEOUT 180)
        add_test(NAME shin-ce-cold-admission
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_cold_admission_test.py
                $<TARGET_FILE:shin-ce> ${CMAKE_CURRENT_SOURCE_DIR}/../profiles)
        set_tests_properties(shin-ce-cold-admission PROPERTIES TIMEOUT 90 COST 45)
        add_executable(shin-local-qualification-driver tests/local_qualification_driver.cpp)
        target_link_libraries(shin-local-qualification-driver PRIVATE shin-observer-native)
        add_test(NAME shin-ce-local-qualification
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_qualification_test.py
                $<TARGET_FILE:shin-ce> ${CMAKE_CURRENT_SOURCE_DIR}/../profiles $<TARGET_FILE:shin-local-qualification-driver>)
        set_tests_properties(shin-ce-local-qualification PROPERTIES TIMEOUT 90)
        add_test(NAME shin-ce-local-resident
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_resident_test.py
                $<TARGET_FILE:shin-ce> ${CMAKE_CURRENT_SOURCE_DIR}/../profiles $<TARGET_FILE:shin-observer-test-core>)
        set_tests_properties(shin-ce-local-resident PROPERTIES TIMEOUT 90)
        add_test(NAME shin-ce-managed-init
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/managed_init_test.py
                $<TARGET_FILE:shin-ce> ${CMAKE_CURRENT_SOURCE_DIR}/../profiles)
        set_tests_properties(shin-ce-managed-init PROPERTIES TIMEOUT 180)
        add_test(NAME shin-ce-local-supervision
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_supervision_test.py
                $<TARGET_FILE:shin-ce> ${CMAKE_CURRENT_SOURCE_DIR}/../profiles)
        set_tests_properties(shin-ce-local-supervision PROPERTIES TIMEOUT 120)
        if(SHIN_OBSERVER_MCP)
            add_test(NAME shin-client-connect
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/client_connect_test.py $<TARGET_FILE:shin-ce>)
            set_tests_properties(shin-client-connect PROPERTIES TIMEOUT 120)
            add_test(NAME shin-client-setup
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/client_setup_test.py
                    $<TARGET_FILE:shin-ce> ${CMAKE_CURRENT_SOURCE_DIR}/../profiles)
            set_tests_properties(shin-client-setup PROPERTIES TIMEOUT 180)
            add_test(NAME shin-native-mcp
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_mcp_test.py
                    $<TARGET_FILE:shin-mcp> $<TARGET_FILE:shin-ce> ${CMAKE_CURRENT_SOURCE_DIR}/../profiles)
            set_tests_properties(shin-native-mcp PROPERTIES TIMEOUT 180)
        endif()
        add_test(NAME shin-ce-local-install
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_install_test.py
                ${CMAKE_COMMAND} ${CMAKE_CURRENT_BINARY_DIR} $<BOOL:${SHIN_OBSERVER_LOCAL_KIT}>)
        set_tests_properties(shin-ce-local-install PROPERTIES TIMEOUT 120 RESOURCE_LOCK shin-install-tree)
        if(SHIN_OBSERVER_LOCAL_KIT)
            add_test(NAME shin-ce-native-installation
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_installation_test.py
                    ${CMAKE_COMMAND} ${CMAKE_CURRENT_BINARY_DIR})
            set_tests_properties(shin-ce-native-installation PROPERTIES TIMEOUT 180 RESOURCE_LOCK shin-install-tree)
            add_executable(shin-update-rpc-tests installer/update_rpc_test.cpp)
            target_link_libraries(shin-update-rpc-tests PRIVATE shin-observer-native)
            add_test(NAME shin-update-rpc COMMAND shin-update-rpc-tests)
            add_test(NAME shin-ce-native-update
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/installer/update_test.py
                    ${CMAKE_COMMAND} ${CMAKE_CURRENT_BINARY_DIR})
            set_tests_properties(shin-ce-native-update PROPERTIES TIMEOUT 180 RESOURCE_LOCK shin-install-tree)
        endif()
        add_executable(shin-observer-test-core ../src/main.cpp)
        target_link_libraries(shin-observer-test-core PRIVATE shin-context-core)
        add_test(NAME shin-ce-local-route
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_route_test.py
                $<TARGET_FILE:shin-ce> $<TARGET_FILE:shin-observer> $<TARGET_FILE:shin-observer-test-core>
                $<TARGET_FILE:shin-local-qualification-driver>)
        set_tests_properties(shin-ce-local-route PROPERTIES TIMEOUT 180)
        # Start the longest isolated lifecycle suites first on a fresh CTest run.
        # Costs only affect scheduling; deadlines and assertions are unchanged.
        set_tests_properties(shin-ce-local-service PROPERTIES COST 130)
        set_tests_properties(shin-ce-local-route PROPERTIES COST 110)
        set_tests_properties(shin-ce-local-watch shin-ce-managed-init PROPERTIES COST 65)
        if(SHIN_OBSERVER_MCP)
            set_tests_properties(shin-native-mcp PROPERTIES COST 90)
        endif()
    endif()
    add_executable(shin-observer-source-view-tests tests/source_view_test.cpp)
    target_link_libraries(shin-observer-source-view-tests PRIVATE shin-observer-native)
    target_compile_options(shin-observer-source-view-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-observer-source-view COMMAND shin-observer-source-view-tests)
    add_executable(shin-observer-io-tests tests/io_test.cpp)
    target_link_libraries(shin-observer-io-tests PRIVATE shin-observer-native)
    target_compile_options(shin-observer-io-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-observer-io COMMAND shin-observer-io-tests)
    # Inject only in this IO copy, after platform system declarations.
    add_library(shin-observer-io-interleaving OBJECT src/io.cpp src/io_json.cpp)
    target_include_directories(shin-observer-io-interleaving PUBLIC include ${CMAKE_CURRENT_SOURCE_DIR}/../shared/observer-protocol/include)
    target_link_libraries(shin-observer-io-interleaving PUBLIC shin-context-core OpenSSL::Crypto)
    target_compile_definitions(shin-observer-io-interleaving PRIVATE SHIN_OBSERVER_TEST_OPENAT=1)
    target_compile_options(shin-observer-io-interleaving PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_executable(shin-observer-io-interleaving-tests tests/io_interleaving_test.cpp)
    target_link_libraries(shin-observer-io-interleaving-tests PRIVATE shin-observer-io-interleaving shin-observer-native)
    if(SHIN_OBSERVER_SOURCE_DISCOVERY)
        target_compile_definitions(shin-observer-io-interleaving-tests PRIVATE SHIN_TEST_LOCAL_CONTROLLER=1)
    endif()
    target_compile_options(shin-observer-io-interleaving-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-observer-io-interleaving COMMAND shin-observer-io-interleaving-tests)
    set_tests_properties(shin-observer-io-interleaving PROPERTIES TIMEOUT 15)
    if(APPLE)
        add_library(shin-observer-identity-io OBJECT src/io.cpp src/io_json.cpp)
        target_include_directories(shin-observer-identity-io PUBLIC include ${CMAKE_CURRENT_SOURCE_DIR}/../shared/observer-protocol/include)
        target_link_libraries(shin-observer-identity-io PUBLIC shin-context-core OpenSSL::Crypto)
        target_compile_definitions(shin-observer-identity-io PRIVATE
            fstat=shin_identity_test_fstat fstatat=shin_identity_test_fstatat
            fgetattrlist=shin_identity_test_fgetattrlist)
        add_executable(shin-observer-identity-tests tests/identity_test.cpp)
        target_link_libraries(shin-observer-identity-tests PRIVATE shin-observer-identity-io)
        add_test(NAME shin-observer-persistent-identity COMMAND shin-observer-identity-tests)
    endif()
    add_executable(shin-observer-outbox-tests tests/outbox_test.cpp)
    target_link_libraries(shin-observer-outbox-tests PRIVATE shin-observer-native)
    target_compile_options(shin-observer-outbox-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-observer-outbox COMMAND shin-observer-outbox-tests)
endif()

if(SHIN_OBSERVER_LOCAL_KIT)
    include(cmake/LocalKit.cmake)
endif()
if(SHIN_OBSERVER_WINDOWS_KIT)
    include(cmake/WindowsKit.cmake)
endif()
