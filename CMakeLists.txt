cmake_minimum_required(VERSION 3.16)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SHIN_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
if(NOT SHIN_VERSION MATCHES "^((0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*))(-(dev|rc)\\.(0|[1-9][0-9]*))?$")
    message(FATAL_ERROR "VERSION must contain a canonical N.N.N, N.N.N-dev.N or N.N.N-rc.N version.")
endif()

set(SHIN_PROJECT_VERSION "${CMAKE_MATCH_1}")
project(ShinContextEngine VERSION "${SHIN_PROJECT_VERSION}" LANGUAGES C CXX)
include("${CMAKE_CURRENT_SOURCE_DIR}/shared/cmake/WindowsBuild.cmake")

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    set(SHIN_CXX_WARNINGS /W4 /permissive- /utf-8)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    add_link_options(/MANIFEST:EMBED "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/shared/windows-io/application.manifest")
else()
    set(SHIN_CXX_WARNINGS -Wall -Wextra -Wpedantic -Wmissing-declarations)
endif()

find_package(Threads REQUIRED)

option(SHIN_ENABLE_SANITIZERS "Build shin-context with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(MSVC AND SHIN_ENABLE_SANITIZERS)
    message(FATAL_ERROR "The combined ASan/UBSan profile is not qualified for MSVC; use the Linux test bench")
endif()
# Source-tree consumers can set SHIN_CONTEXT_BUILD_CLI=OFF, call add_subdirectory()
# for this root, then link their target to shin-context-core without an install step.
option(SHIN_CONTEXT_BUILD_CLI "Build the shin-context CLI executable and its CTest suite" ON)
option(SHIN_CONTEXT_BUILD_SEMANTIC_EXPORTER "Build the optional offline semantic source exporter" OFF)
option(SHIN_CONTEXT_BUILD_SOURCE_DISCOVERY "Build the explicit native source-candidate provider (SQLite FTS5)" ${WIN32})

add_library(shin-syntax-vendor STATIC
    third_party/tree-sitter/src/lib.c
    third_party/tree-sitter-java/src/parser.c
    third_party/tree-sitter-cpp/src/parser.c
    third_party/tree-sitter-cpp/src/scanner.c
    third_party/tree-sitter-typescript/typescript/src/parser.c
    third_party/tree-sitter-typescript/typescript/src/scanner.c
    third_party/tree-sitter-typescript/tsx/src/parser.c
    third_party/tree-sitter-typescript/tsx/src/scanner.c
)
set_target_properties(shin-syntax-vendor PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(shin-syntax-vendor PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE=1)
if(MSVC)
    target_compile_options(shin-syntax-vendor PRIVATE /utf-8 /we4013)
else()
    target_compile_options(shin-syntax-vendor PRIVATE -Werror=implicit-function-declaration)
endif()
target_include_directories(shin-syntax-vendor SYSTEM
    PUBLIC third_party/tree-sitter/include
    PRIVATE third_party/tree-sitter/src)
set_source_files_properties(third_party/tree-sitter-cpp/src/scanner.c
    PROPERTIES COMPILE_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-cpp/src")
foreach(SYNTAX_GRAMMAR typescript tsx)
    set_source_files_properties(third_party/tree-sitter-typescript/${SYNTAX_GRAMMAR}/src/scanner.c
        PROPERTIES COMPILE_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-typescript/${SYNTAX_GRAMMAR}/src")
endforeach()

set_property(SOURCE
    third_party/tree-sitter-cpp/src/scanner.c
    third_party/tree-sitter-typescript/typescript/src/scanner.c
    third_party/tree-sitter-typescript/tsx/src/scanner.c
    APPEND PROPERTY COMPILE_OPTIONS "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-Werror=strict-prototypes>")

set(SHIN_CONTEXT_CORE_SOURCES
    src/cli.cpp
    src/adoption.cpp
    src/adoption_plan.cpp
    src/authority.cpp
    src/core.cpp
    src/profiles.cpp
    src/profile_policy.cpp
    src/extraction.cpp
    src/identifier_extraction.cpp
    src/evidence.cpp
    src/retool_extraction.cpp
    src/generation.cpp
    src/compact_terms.cpp
    src/generated_row_decoder.cpp
    src/generated_snapshot.cpp
    src/retrieval.cpp
    src/lexical_matching.cpp
    src/document_candidates.cpp
    src/scoped_retrieval.cpp
    src/source_relations.cpp
    src/source_syntax.cpp
    src/syntax_source_facts.cpp
    src/impact.cpp
    src/execution_commands.cpp
    src/execution_inputs.cpp
    src/execution_io.cpp
    src/execution_receipts.cpp
    src/execution_report.cpp
    src/execution_report_junit.cpp
    src/execution_report_playwright.cpp
    src/planned_retrieval.cpp
    src/query_coverage.cpp
    src/brief_compact_render.cpp
    src/retrieval_session.cpp
    src/inspect.cpp
    src/read.cpp
    src/source_read.cpp
    src/task_request.cpp
    src/agent_context_pack.cpp
    src/agent_context_pack_render.cpp
    src/pack_command.cpp
    src/pack_diff.cpp
    src/pack_bench.cpp
    src/pack_eval.cpp
    src/batch.cpp
    src/diagnose.cpp
    src/worktree_diff.cpp
    src/worktree_state.cpp
    src/session_input.cpp
    src/session_contract.cpp
    src/session_checkpoint.cpp
    src/session_resume.cpp
    src/snapshot_verification.cpp
    src/snapshot_diff.cpp
    src/federation.cpp
    src/federated_brief.cpp
    src/federated_eval.cpp
    src/profile_suggest.cpp
    src/index_lifecycle.cpp
    src/ready.cpp
    src/index.cpp
    src/generation_admission.cpp
    src/health.cpp
    src/commands.cpp
    src/bench.cpp
    src/eval.cpp
    src/validate.cpp
    src/validation_generation.cpp
    src/generated_io.cpp
    third_party/simdjson/simdjson.cpp
    third_party/pugixml/pugixml.cpp
)
add_library(shin-context-core STATIC ${SHIN_CONTEXT_CORE_SOURCES})
if(WIN32)
    add_subdirectory(shared/windows-io)
    target_link_libraries(shin-context-core PRIVATE shin-windows-io)
endif()
target_include_directories(shin-context-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(shin-context-core SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(shin-context-core PUBLIC Threads::Threads shin-syntax-vendor)
target_compile_definitions(shin-context-core PUBLIC SHIN_VERSION="${SHIN_VERSION}")
target_compile_options(shin-context-core PRIVATE ${SHIN_CXX_WARNINGS})
include(shared/cmake/CoreAllocator.cmake)
if(SHIN_CONTEXT_BUILD_CLI)
    add_executable(shin-context src/main.cpp)
    target_link_libraries(shin-context PRIVATE shin-context-core)
    target_compile_options(shin-context PRIVATE ${SHIN_CXX_WARNINGS})
    if(SHIN_CORE_ALLOCATOR STREQUAL "mimalloc")
        shin_link_core_allocator(shin-context)
    endif()
endif()
set_source_files_properties(
    third_party/simdjson/simdjson.cpp
    third_party/pugixml/pugixml.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-missing-declarations>"
)

include(ProcessorCount)
ProcessorCount(SHIN_CLANG_TIDY_JOBS)
if(NOT SHIN_CLANG_TIDY_JOBS)
    set(SHIN_CLANG_TIDY_JOBS 1)
endif()
find_program(
    SHIN_CLANG_TIDY_EXECUTABLE
    NAMES clang-tidy-23 clang-tidy
    HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
)
find_program(
    SHIN_RUN_CLANG_TIDY_EXECUTABLE
    NAMES run-clang-tidy-23 run-clang-tidy-23.py run-clang-tidy run-clang-tidy.py
    HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
)
if(SHIN_CONTEXT_BUILD_CLI AND SHIN_CLANG_TIDY_EXECUTABLE AND SHIN_RUN_CLANG_TIDY_EXECUTABLE)
    add_custom_target(
        shin-clang-tidy
        COMMAND
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_clang_tidy.sh
            ${SHIN_CLANG_TIDY_EXECUTABLE}
            ${SHIN_RUN_CLANG_TIDY_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${SHIN_CLANG_TIDY_JOBS}
        DEPENDS shin-context
        USES_TERMINAL
        VERBATIM
    )
endif()

set(SHIN_GENERIC_PROFILES
    profiles/generic-cpp.json
    profiles/generic-docs.json
    profiles/generic-dotnet.json
    profiles/generic-java.json
    profiles/generic-kotlin.json
    profiles/generic-node.json
    profiles/generic-retool.json
    profiles/generic-source.json
)
set(SHIN_PORTABLE_CONTRACTS
    docs/machine/adoption-planner-v1.json
    docs/machine/agent-readiness-v1.json
    docs/machine/aggregate-validation-v1.json
    docs/machine/agent-context-pack-diff-v1.json
    docs/machine/agent-context-pack-v1.json
    docs/machine/batch-retrieval-v1.json
    docs/machine/cold-start-bootstrap-v1.json
    docs/machine/evidence-provenance-v1.json
    docs/machine/federated-brief-v1.json
    docs/machine/federated-eval-v1.json
    docs/machine/hermes-agent-integration-v1.json
    docs/machine/identifier-evidence-v1.json
    docs/machine/index-lifecycle-v1.json
    docs/machine/portable-distribution-v1.json
    docs/machine/project-adoption-v1.json
    docs/machine/profile-routing-v1.json
    docs/machine/retrieval-diagnose-v1.json
    docs/machine/retrieval-evidence-v1.json
    docs/machine/lexical-retrieval-v1.json
    docs/machine/retrieval-observation-v1.json
    docs/machine/retool-source-extractor-v1.json
    docs/machine/snapshot-diff-v1.json
    docs/machine/universality-boundary-v1.json
    docs/machine/worktree-diff-v1.json
    docs/machine/session-checkpoint-v1.json
    docs/machine/execution-receipt-v1.json
    docs/machine/execution-evidence-v1.json
    docs/machine/trust-boundary-hardening-v1.json
    docs/machine/change-aware-impact-pack-v1.json
    docs/machine/source-member-ranges-v1.json
    docs/machine/source-read-v1.json
    docs/machine/source-file-read-v1.json
)

if(SHIN_CONTEXT_BUILD_CLI)
    install(TARGETS shin-context RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES VERSION DESTINATION "${CMAKE_INSTALL_DATADIR}/shin-context")
    install(FILES LICENSE NOTICE DESTINATION "${CMAKE_INSTALL_DATADIR}/shin-context")
    install(FILES ${SHIN_GENERIC_PROFILES}
            DESTINATION "${CMAKE_INSTALL_DATADIR}/shin-context/profiles")
    install(FILES ${SHIN_PORTABLE_CONTRACTS}
            DESTINATION "${CMAKE_INSTALL_DATADIR}/shin-context/contracts")
    install(FILES third_party/nlohmann/LICENSE.MIT
            DESTINATION "${CMAKE_INSTALL_DATADIR}/shin-context/licenses/nlohmann")
    install(FILES third_party/simdjson/LICENSE-APACHE-2.0 third_party/simdjson/LICENSE-MIT
            DESTINATION "${CMAKE_INSTALL_DATADIR}/shin-context/licenses/simdjson")
    foreach(SYNTAX_DEP tree-sitter tree-sitter-java tree-sitter-typescript tree-sitter-cpp)
        install(FILES third_party/${SYNTAX_DEP}/LICENSE third_party/${SYNTAX_DEP}/UPSTREAM.json
                DESTINATION "${CMAKE_INSTALL_DATADIR}/shin-context/licenses/${SYNTAX_DEP}")
    endforeach()
    install(FILES third_party/tree-sitter/src/unicode/LICENSE
            DESTINATION "${CMAKE_INSTALL_DATADIR}/shin-context/licenses/tree-sitter/unicode")
    install(FILES third_party/pugixml/LICENSE.MIT third_party/pugixml/UPSTREAM.json
            DESTINATION "${CMAKE_INSTALL_DATADIR}/shin-context/licenses/pugixml")
    install(DIRECTORY integrations/codex/skills/shin-context
            DESTINATION "${CMAKE_INSTALL_DATADIR}/shin-context/codex/skills")
    install(DIRECTORY integrations/hermes/plugins/shin-context
            DESTINATION "${CMAKE_INSTALL_DATADIR}/shin-context/hermes/plugins"
            PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE PATTERN "*.pyo" EXCLUDE)
endif()

if(SHIN_CONTEXT_BUILD_SEMANTIC_EXPORTER)
    add_executable(shin-semantic-export tools/semantic_source_export.cpp)
    target_link_libraries(shin-semantic-export PRIVATE shin-context-core)
    target_compile_options(shin-semantic-export PRIVATE ${SHIN_CXX_WARNINGS})
endif()

include(CTest)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(SHIN_CONTEXT_BUILD_OBSERVER "Build the native Observer/CE product" ${WIN32})
    if(SHIN_CONTEXT_BUILD_OBSERVER)
        set(SHIN_OBSERVER_SOURCE_DISCOVERY ${SHIN_CONTEXT_BUILD_SOURCE_DISCOVERY})
        option(SHIN_OBSERVER_MCP "Build the native MCP adapter" ON)
        include("${CMAKE_CURRENT_SOURCE_DIR}/observer/cmake/Native.cmake")
        if(WIN32 AND BUILD_TESTING AND SHIN_OBSERVER_SOURCE_DISCOVERY)
            add_subdirectory(observer/platform/windows)
        endif()
    endif()
endif()
if(SHIN_CONTEXT_BUILD_SOURCE_DISCOVERY)
    include("${CMAKE_CURRENT_SOURCE_DIR}/shared/cmake/SQLite.cmake")
    if(WIN32)
        set(SHIN_SOURCE_CACHE src/source_cache_windows.cpp)
    else()
        set(SHIN_SOURCE_CACHE src/source_cache.cpp)
    endif()
    add_library(shin-source-discovery-provider STATIC src/source_discovery.cpp ${SHIN_SOURCE_CACHE} src/source_context.cpp)
    if(WIN32)
        target_link_libraries(shin-source-discovery-provider PRIVATE shin-windows-io)
        target_compile_definitions(shin-source-discovery-provider PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(TARGET SQLite3::SQLite3)
        target_link_libraries(shin-source-discovery-provider PUBLIC shin-context-core PRIVATE SQLite3::SQLite3)
    else()
        target_link_libraries(shin-source-discovery-provider PUBLIC shin-context-core PRIVATE SQLite::SQLite3)
    endif()
    target_compile_options(shin-source-discovery-provider PRIVATE ${SHIN_CXX_WARNINGS})
    add_executable(shin-source-discovery tools/source_discovery.cpp)
    target_link_libraries(shin-source-discovery PRIVATE shin-source-discovery-provider)
    if(BUILD_TESTING)
        add_executable(shin-source-discovery-tests tests/source_discovery_test.cpp)
        target_link_libraries(shin-source-discovery-tests PRIVATE shin-source-discovery-provider)
        if(WIN32)
            target_link_libraries(shin-source-discovery-tests PRIVATE advapi32)
        endif()
        add_executable(shin-source-discovery-reuse-bench tests/source_discovery_reuse_bench.cpp)
        target_link_libraries(shin-source-discovery-reuse-bench PRIVATE shin-source-discovery-provider)
        add_executable(shin-resident-storage-bench tests/resident_storage_bench.cpp)
        target_link_libraries(shin-resident-storage-bench PRIVATE shin-source-discovery-provider)
        add_test(NAME shin-context-source-discovery COMMAND shin-source-discovery-tests ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(shin-context-source-discovery PROPERTIES TIMEOUT 60)
    endif()
endif()
if(BUILD_TESTING AND SHIN_CONTEXT_BUILD_CLI)
    add_executable(shin-generation-admission-tests tests/generation_admission_test.cpp)
    target_link_libraries(shin-generation-admission-tests PRIVATE shin-context-core)
    add_test(NAME shin-context-generation-admission COMMAND shin-generation-admission-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/profiles)
    set_tests_properties(shin-context-generation-admission PROPERTIES TIMEOUT 30)
    add_executable(shin-validation-generation-tests tests/validation_generation_test.cpp)
    target_link_libraries(shin-validation-generation-tests PRIVATE shin-context-core)
    add_test(NAME shin-context-validation-generation COMMAND shin-validation-generation-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/profiles)
    set_tests_properties(shin-context-validation-generation PROPERTIES TIMEOUT 60)
    add_executable(shin-source-chunking-tests tests/source_chunking_test.cpp)
    target_link_libraries(shin-source-chunking-tests PRIVATE shin-context-core)
    target_compile_options(shin-source-chunking-tests PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-source-chunking COMMAND shin-source-chunking-tests)
    set_tests_properties(shin-context-source-chunking PROPERTIES TIMEOUT 30)
    if(SHIN_CORE_ALLOCATOR STREQUAL "mimalloc")
        add_executable(shin-core-allocator-tests tests/core_allocator_test.cpp)
        shin_link_core_allocator(shin-core-allocator-tests)
        target_link_libraries(shin-core-allocator-tests PRIVATE Threads::Threads)
        add_test(NAME shin-context-allocator COMMAND shin-core-allocator-tests)
        set_tests_properties(shin-context-allocator PROPERTIES TIMEOUT 15)
    endif()
    if(UNIX)
        find_program(BASH_EXECUTABLE bash REQUIRED)
        find_program(JQ_EXECUTABLE jq REQUIRED)
    endif()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    if(WIN32)
        if(SHIN_CONTEXT_BUILD_SOURCE_DISCOVERY)
            add_test(NAME shin-context-windows-source-cache
                COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_windows_source_cache.py
                    $<TARGET_FILE:shin-context> $<TARGET_FILE:shin-source-discovery> ${CMAKE_CURRENT_SOURCE_DIR}/profiles)
            set_tests_properties(shin-context-windows-source-cache PROPERTIES TIMEOUT 90)
        endif()
        add_test(NAME shin-context-windows-smoke
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_windows_core_smoke.py
                $<TARGET_FILE:shin-context> ${CMAKE_CURRENT_SOURCE_DIR}/profiles)
        set_tests_properties(shin-context-windows-smoke PROPERTIES TIMEOUT 90)
    endif()
    if(SHIN_CONTEXT_BUILD_SOURCE_DISCOVERY)
        add_test(NAME shin-context-transfer
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_context_transfer.py
                --core $<TARGET_FILE:shin-context> --discovery $<TARGET_FILE:shin-source-discovery>)
        set_tests_properties(shin-context-transfer PROPERTIES TIMEOUT 60)
    endif()
    add_test(NAME shin-context-allocator-config
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_core_allocator_config.py)
    set_tests_properties(shin-context-allocator-config PROPERTIES TIMEOUT 30)
    add_test(NAME shin-context-retool-language-composition
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_retool_language_composition.py
                $<TARGET_FILE:shin-context> ${CMAKE_CURRENT_SOURCE_DIR}/profiles)
    add_test(NAME shin-context-golden-replay
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_consumer_golden_replay.py)
    add_test(NAME shin-context-agent-workflow-audit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_agent_workflow_probe.py)
    if(UNIX)
        add_test(NAME shin-context-eval-review
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_eval_review.py)
        set_tests_properties(shin-context-eval-review PROPERTIES TIMEOUT 30)
        add_test(NAME shin-context-eval-resource-policy
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_eval_resource_policy.py
                    $<TARGET_FILE:shin-context>)
        set_tests_properties(shin-context-eval-resource-policy PROPERTIES TIMEOUT 120)
        add_test(NAME shin-context-standard-retrieval-reference
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_standard_retrieval.py)
        set_tests_properties(shin-context-standard-retrieval-reference PROPERTIES TIMEOUT 30)
        add_test(NAME shin-context-cross-encoder-reference
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cross_encoder_reference.py)
        set_tests_properties(shin-context-cross-encoder-reference PROPERTIES TIMEOUT 30)
        add_test(NAME shin-context-hierarchical-retrieval
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_hierarchical_retrieval.py)
        set_tests_properties(shin-context-hierarchical-retrieval PROPERTIES TIMEOUT 30)
        add_test(NAME shin-context-agent-query-projection
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compact_query.py)
        set_tests_properties(shin-context-agent-query-projection PROPERTIES TIMEOUT 30)
        add_test(NAME shin-context-agent-query-pilot-audit
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_agent_query_pilot.py)
        set_tests_properties(shin-context-agent-query-pilot-audit PROPERTIES TIMEOUT 30)
        add_test(NAME shin-context-code-context-workflow
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_code_context_workflow.py
                    --core $<TARGET_FILE:shin-context>)
        set_tests_properties(shin-context-code-context-workflow PROPERTIES TIMEOUT 60)
        add_test(NAME shin-context-code-context-pair
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_code_context_pair.py
                    --core $<TARGET_FILE:shin-context>)
        set_tests_properties(shin-context-code-context-pair PROPERTIES TIMEOUT 60)
        add_test(NAME shin-context-code-context-pair-review
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_code_context_pair_review.py)
        set_tests_properties(shin-context-code-context-pair-review PROPERTIES TIMEOUT 30)
        add_test(NAME shin-context-answer-grounding
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_answer_grounding.py)
        set_tests_properties(shin-context-answer-grounding PROPERTIES TIMEOUT 30)
        add_test(NAME shin-context-local-query-metrics
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_query_metrics.py)
        set_tests_properties(shin-context-local-query-metrics PROPERTIES TIMEOUT 30)
        add_test(NAME shin-context-project-portfolio
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_portfolio.py)
        set_tests_properties(shin-context-project-portfolio PROPERTIES TIMEOUT 30)
        add_test(NAME shin-context-project-dashboard
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dashboard.py)
        set_tests_properties(shin-context-project-dashboard PROPERTIES TIMEOUT 30)
        add_test(NAME shin-context-model-usage
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_model_usage.py)
        set_tests_properties(shin-context-model-usage PROPERTIES TIMEOUT 30)
        add_test(NAME shin-context-client-status-hook
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_client_status_hook.py)
        # This suite measures a real one-second child deadline. Keep CPU contention
        # from unrelated suites out of the measurement; do not relax that deadline.
        set_tests_properties(shin-context-client-status-hook PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
        add_test(NAME shin-context-semantic-runtime
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_semantic_runtime.py
                    --core $<TARGET_FILE:shin-context>)
        set_tests_properties(shin-context-semantic-runtime PROPERTIES TIMEOUT 60)
        if(SHIN_CONTEXT_BUILD_SEMANTIC_EXPORTER)
            add_test(NAME shin-context-semantic-current-source-export
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_semantic_source_export.py
                        $<TARGET_FILE:shin-context> $<TARGET_FILE:shin-semantic-export>)
            add_test(NAME shin-context-semantic-producer
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_semantic_producer.py
                        --core $<TARGET_FILE:shin-context> --exporter $<TARGET_FILE:shin-semantic-export>)
            set_tests_properties(shin-context-semantic-current-source-export shin-context-semantic-producer PROPERTIES TIMEOUT 120)
        endif()
    endif()
    add_test(NAME shin-context-project-search-comparison
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_project_search_comparison.py)
    add_test(NAME shin-context-local-semantic-pilot
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_local_semantic_pilot.py)
    add_executable(shin-semantic-source-export tests/semantic_source_export.cpp)
    target_link_libraries(shin-semantic-source-export PRIVATE shin-context-core)
    target_compile_options(shin-semantic-source-export PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-semantic-source-export
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_semantic_source_export.py
                $<TARGET_FILE:shin-context> $<TARGET_FILE:shin-semantic-source-export>)
    set_tests_properties(shin-context-semantic-source-export PROPERTIES TIMEOUT 90)
    add_executable(shin-semantic-lexical-export tests/semantic_lexical_export.cpp)
    target_link_libraries(shin-semantic-lexical-export PRIVATE shin-context-core)
    target_compile_options(shin-semantic-lexical-export PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-semantic-hybrid
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_semantic_hybrid.py
                $<TARGET_FILE:shin-context> $<TARGET_FILE:shin-semantic-lexical-export>)
    set_tests_properties(shin-context-semantic-hybrid PROPERTIES TIMEOUT 90)
    add_test(NAME shin-context-semantic-source-hop
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_semantic_source_hop.py)
    set_tests_properties(shin-context-semantic-source-hop PROPERTIES TIMEOUT 30)
    add_test(NAME shin-context-semantic-miss-review
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_semantic_miss_review.py)
    set_tests_properties(shin-context-semantic-miss-review PROPERTIES TIMEOUT 30)
    add_test(NAME shin-context-release-contract-gate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release_contract_gate.py)
    set_tests_properties(shin-context-release-contract-gate PROPERTIES TIMEOUT 120)
    add_test(NAME shin-context-self-profile
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_self_profile_tests.py
                $<TARGET_FILE:shin-context> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(shin-context-self-profile PROPERTIES TIMEOUT 120)
    add_test(NAME shin-context-profile-policy-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_profile_policy.py
                $<TARGET_FILE:shin-context> ${CMAKE_CURRENT_SOURCE_DIR}/profiles)
    set_tests_properties(shin-context-profile-policy-cli PROPERTIES TIMEOUT 90)
    add_executable(shin-profile-policy-tests tests/profile_policy_test.cpp)
    target_link_libraries(shin-profile-policy-tests PRIVATE shin-context-core)
    target_compile_options(shin-profile-policy-tests PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-profile-policy-api
        COMMAND $<TARGET_FILE:shin-profile-policy-tests> ${CMAKE_CURRENT_BINARY_DIR})
    add_executable(shin-secret-assignment-tests tests/secret_assignment_test.cpp)
    target_link_libraries(shin-secret-assignment-tests PRIVATE shin-context-core)
    target_compile_options(shin-secret-assignment-tests PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-secret-assignment COMMAND shin-secret-assignment-tests)
    add_executable(shin-scanner-prototype-tests tests/scanner_prototypes.c)
    set_target_properties(shin-scanner-prototype-tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_link_libraries(shin-scanner-prototype-tests PRIVATE shin-syntax-vendor)
    add_test(NAME shin-context-scanner-prototypes COMMAND shin-scanner-prototype-tests)
    set_tests_properties(shin-context-scanner-prototypes PROPERTIES TIMEOUT 30)
    add_test(NAME shin-context-identifier-query
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/identifier-query/test_identifier_query.py
                $<TARGET_FILE:shin-context>)
    set_tests_properties(shin-context-identifier-query PROPERTIES TIMEOUT 90)
    add_test(NAME shin-context-lexical-retrieval
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/lexical-retrieval/test_lexical_retrieval.py
                $<TARGET_FILE:shin-context>)
    set_tests_properties(shin-context-lexical-retrieval PROPERTIES TIMEOUT 90)
    add_test(NAME shin-context-query-parity
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/query-parity/test_query_parity.py
                $<TARGET_FILE:shin-context>)
    set_tests_properties(shin-context-query-parity PROPERTIES TIMEOUT 90)
    add_test(NAME shin-context-scoped-retrieval
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/scoped-retrieval/test_scoped_retrieval.py
                $<TARGET_FILE:shin-context>)
    set_tests_properties(shin-context-scoped-retrieval PROPERTIES TIMEOUT 90)
    add_test(NAME shin-context-planner-contract
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/planner-contract/test_planner_contract.py
                $<TARGET_FILE:shin-context>)
    set_tests_properties(shin-context-planner-contract PROPERTIES TIMEOUT 90)
    add_executable(shin-execution-report-tests
        tests/fixtures/execution-report/runner.cpp
        src/execution_report.cpp src/execution_report_junit.cpp src/execution_report_playwright.cpp
        third_party/pugixml/pugixml.cpp third_party/simdjson/simdjson.cpp)
    target_include_directories(shin-execution-report-tests PRIVATE src)
    target_include_directories(shin-execution-report-tests SYSTEM PRIVATE third_party)
    target_compile_options(shin-execution-report-tests PRIVATE ${SHIN_CXX_WARNINGS})
    target_compile_definitions(shin-execution-report-tests PRIVATE SHIN_VERSION="${SHIN_VERSION}")
    add_test(NAME shin-context-execution-report
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_execution_report_tests.sh
                $<TARGET_FILE:shin-execution-report-tests> ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(shin-context-execution-report PROPERTIES TIMEOUT 30)
    add_executable(shin-compact-terms-tests tests/compact_terms_test.cpp)
    target_link_libraries(shin-compact-terms-tests PRIVATE shin-context-core)
    target_compile_options(shin-compact-terms-tests PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-compact-terms COMMAND shin-compact-terms-tests)
    set_tests_properties(shin-context-compact-terms PROPERTIES TIMEOUT 60)
    add_executable(shin-warm-snapshot-tests tests/warm_snapshot_test.cpp)
    add_executable(shin-knowledge-vacuum-tests tests/knowledge_vacuum_test.cpp)
    add_executable(shin-publisher-ownership-tests tests/publisher_ownership_test.cpp)
    target_link_libraries(shin-publisher-ownership-tests PRIVATE shin-context-core)
    if(WIN32)
        target_link_libraries(shin-publisher-ownership-tests PRIVATE advapi32)
    endif()
    target_compile_options(shin-publisher-ownership-tests PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-publisher-ownership COMMAND shin-publisher-ownership-tests)
    set_tests_properties(shin-context-publisher-ownership PROPERTIES TIMEOUT 20)
    target_link_libraries(shin-knowledge-vacuum-tests PRIVATE shin-context-core)
    target_compile_options(shin-knowledge-vacuum-tests PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-knowledge-vacuum COMMAND shin-knowledge-vacuum-tests ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(shin-context-knowledge-vacuum PROPERTIES TIMEOUT 60)
    target_link_libraries(shin-warm-snapshot-tests PRIVATE shin-context-core)
    target_compile_options(shin-warm-snapshot-tests PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-warm-snapshot
        COMMAND $<TARGET_FILE:shin-warm-snapshot-tests> ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(shin-context-warm-snapshot PROPERTIES TIMEOUT 60)
    add_executable(shin-qualified-query-tests tests/qualified_query_test.cpp)
    target_link_libraries(shin-qualified-query-tests PRIVATE shin-context-core)
    target_compile_options(shin-qualified-query-tests PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-qualified-query
        COMMAND $<TARGET_FILE:shin-qualified-query-tests> ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(shin-context-qualified-query PROPERTIES TIMEOUT 60)
    add_executable(shin-inspect-tests tests/inspect_test.cpp)
    add_executable(shin-cpp-syntax-tests tests/cpp_syntax_test.cpp)
    target_link_libraries(shin-cpp-syntax-tests PRIVATE shin-context-core)
    target_compile_options(shin-cpp-syntax-tests PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-cpp-syntax COMMAND shin-cpp-syntax-tests)
    set_tests_properties(shin-context-cpp-syntax PROPERTIES TIMEOUT 30)
    target_link_libraries(shin-inspect-tests PRIVATE shin-context-core)
    target_compile_options(shin-inspect-tests PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-inspect
        COMMAND $<TARGET_FILE:shin-inspect-tests> ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(shin-context-inspect PROPERTIES TIMEOUT 60)
    add_executable(shin-source-read-tests tests/source_read_test.cpp)
    target_link_libraries(shin-source-read-tests PRIVATE shin-context-core)
    target_compile_options(shin-source-read-tests PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-source-read
        COMMAND $<TARGET_FILE:shin-source-read-tests> ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(shin-context-source-read PROPERTIES TIMEOUT 90)
    add_executable(shin-source-observation-tests tests/source_observation_test.cpp)
    target_link_libraries(shin-source-observation-tests PRIVATE shin-context-core)
    target_compile_options(shin-source-observation-tests PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-source-observation
        COMMAND $<TARGET_FILE:shin-source-observation-tests> ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(shin-context-source-observation PROPERTIES TIMEOUT 30)
    add_test(NAME shin-context-source-read-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_source_read_cli.py
                $<TARGET_FILE:shin-context>)
    set_tests_properties(shin-context-source-read-cli PROPERTIES TIMEOUT 90)
    add_executable(shin-retrieval-scoring-tests tests/retrieval_scoring_test.cpp)
    target_link_libraries(shin-retrieval-scoring-tests PRIVATE shin-context-core)
    target_compile_options(shin-retrieval-scoring-tests PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-retrieval-scoring COMMAND $<TARGET_FILE:shin-retrieval-scoring-tests>)
    set_tests_properties(shin-context-retrieval-scoring PROPERTIES TIMEOUT 30)
    add_executable(shin-profile-quality-tests tests/profile_quality_test.cpp)
    target_link_libraries(shin-profile-quality-tests PRIVATE shin-context-core)
    target_compile_options(shin-profile-quality-tests PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-profile-quality
        COMMAND $<TARGET_FILE:shin-profile-quality-tests> ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(shin-context-profile-quality PROPERTIES TIMEOUT 60)
    add_executable(shin-evaluation-bench EXCLUDE_FROM_ALL tests/evaluation_bench.cpp)
    target_link_libraries(shin-evaluation-bench PRIVATE shin-context-core)
    target_compile_options(shin-evaluation-bench PRIVATE ${SHIN_CXX_WARNINGS})
    add_executable(shin-source-chunk-reuse-bench EXCLUDE_FROM_ALL tests/source_chunk_reuse_bench.cpp)
    target_link_libraries(shin-source-chunk-reuse-bench PRIVATE shin-context-core)
    target_compile_options(shin-source-chunk-reuse-bench PRIVATE ${SHIN_CXX_WARNINGS})
    # Override only this translation unit in the optional executable. The archive's
    # production lexical_matching object is not pulled into this experiment.
    add_executable(shin-lexical-reuse-bench EXCLUDE_FROM_ALL
        tests/lexical_reuse_bench.cpp src/lexical_matching.cpp)
    target_include_directories(shin-lexical-reuse-bench PRIVATE tests)
    target_compile_definitions(shin-lexical-reuse-bench PRIVATE SHIN_EXPERIMENT_LEXICAL_REUSE=1)
    target_link_libraries(shin-lexical-reuse-bench PRIVATE shin-context-core)
    target_compile_options(shin-lexical-reuse-bench PRIVATE ${SHIN_CXX_WARNINGS})
    add_executable(shin-field-preparation-bench EXCLUDE_FROM_ALL
        tests/field_preparation_bench.cpp tests/field_preparation_experiment.cpp src/retrieval.cpp)
    target_include_directories(shin-field-preparation-bench PRIVATE tests)
    target_compile_definitions(shin-field-preparation-bench PRIVATE SHIN_EXPERIMENT_FIELD_PREPARATION=1)
    target_link_libraries(shin-field-preparation-bench PRIVATE shin-context-core)
    target_compile_options(shin-field-preparation-bench PRIVATE ${SHIN_CXX_WARNINGS})
    add_executable(shin-row-batch-bench EXCLUDE_FROM_ALL
        tests/row_batch_bench.cpp tests/row_batch_experiment.cpp src/retrieval.cpp)
    target_include_directories(shin-row-batch-bench PRIVATE tests)
    target_compile_definitions(shin-row-batch-bench PRIVATE SHIN_EXPERIMENT_ROW_BATCH=1)
    target_link_libraries(shin-row-batch-bench PRIVATE shin-context-core)
    target_compile_options(shin-row-batch-bench PRIVATE ${SHIN_CXX_WARNINGS})
    add_executable(shin-eval-batch-oracle EXCLUDE_FROM_ALL tests/eval_batch_oracle.cpp)
    target_link_libraries(shin-eval-batch-oracle PRIVATE shin-context-core)
    target_compile_options(shin-eval-batch-oracle PRIVATE ${SHIN_CXX_WARNINGS})
    add_executable(shin-eval-batch-bench EXCLUDE_FROM_ALL
        tests/eval_batch_bench.cpp tests/eval_batch_experiment.cpp tests/row_batch_experiment.cpp
        src/retrieval.cpp src/commands.cpp src/eval.cpp)
    target_include_directories(shin-eval-batch-bench PRIVATE tests)
    target_compile_definitions(shin-eval-batch-bench PRIVATE
        SHIN_EXPERIMENT_EVAL_BATCH=1 SHIN_EXPERIMENT_ROW_BATCH=1)
    target_link_libraries(shin-eval-batch-bench PRIVATE shin-context-core)
    target_compile_options(shin-eval-batch-bench PRIVATE ${SHIN_CXX_WARNINGS})
    foreach(AGGREGATE_TARGET bench test)
        if(AGGREGATE_TARGET STREQUAL "test")
            add_executable(shin-aggregate-recovery-test
                tests/aggregate_recovery_test.cpp src/commands.cpp src/retrieval.cpp)
            add_test(NAME shin-context-aggregate-recovery
                COMMAND $<TARGET_FILE:shin-aggregate-recovery-test> ${CMAKE_CURRENT_BINARY_DIR})
            set_tests_properties(shin-context-aggregate-recovery PROPERTIES TIMEOUT 120)
        else()
            add_executable(shin-aggregate-recovery-bench EXCLUDE_FROM_ALL
                tests/aggregate_recovery_bench.cpp src/commands.cpp src/retrieval.cpp)
        endif()
        target_include_directories(shin-aggregate-recovery-${AGGREGATE_TARGET} PRIVATE tests)
        target_compile_definitions(shin-aggregate-recovery-${AGGREGATE_TARGET} PRIVATE SHIN_EXPERIMENT_AGGREGATE_RECOVERY=1)
        target_link_libraries(shin-aggregate-recovery-${AGGREGATE_TARGET} PRIVATE shin-context-core)
        target_compile_options(shin-aggregate-recovery-${AGGREGATE_TARGET} PRIVATE ${SHIN_CXX_WARNINGS})
    endforeach()
    add_executable(shin-warm-snapshot-bench tests/warm_snapshot_bench.cpp)
    target_link_libraries(shin-warm-snapshot-bench PRIVATE shin-context-core)
    target_compile_options(shin-warm-snapshot-bench PRIVATE ${SHIN_CXX_WARNINGS})
    foreach(SHIN_FIXTURE trust_boundary pack_provenance)
        add_test(NAME shin-context-${SHIN_FIXTURE}
            COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_${SHIN_FIXTURE}_tests.sh
                    ${JQ_EXECUTABLE} $<TARGET_FILE:shin-context> ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(shin-context-${SHIN_FIXTURE} PROPERTIES TIMEOUT 60)
    endforeach()
    add_test(
        NAME shin-context-authority
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_authority_tests.sh
                ${JQ_EXECUTABLE} $<TARGET_FILE:shin-context> ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(shin-context-authority PROPERTIES TIMEOUT 60)
    foreach(SHIN_FIXTURE member_range source_relation impact execution_evidence)
        add_test(NAME shin-context-${SHIN_FIXTURE}
            COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_${SHIN_FIXTURE}_tests.sh
                    $<TARGET_FILE:shin-context> ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(shin-context-${SHIN_FIXTURE} PROPERTIES TIMEOUT 120
            ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}")
    endforeach()
    add_executable(shin-session-evaluation-fixture tests/session_evaluation_fixture.cpp)
    target_link_libraries(shin-session-evaluation-fixture PRIVATE shin-context-core)
    target_compile_options(shin-session-evaluation-fixture PRIVATE ${SHIN_CXX_WARNINGS})
    add_test(NAME shin-context-session-evaluation
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_session_evaluation_tests.py
                $<TARGET_FILE:shin-context> $<TARGET_FILE:shin-session-evaluation-fixture> ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(shin-context-session-evaluation PROPERTIES TIMEOUT 120)
    add_test(
        NAME shin-context-fixtures
        COMMAND ${BASH_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_fixture_tests.sh
                $<TARGET_FILE:shin-context>
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    if(SHIN_ENABLE_SANITIZERS)
        # Leak inspection repeats for each CLI child in this integration suite.
        set_tests_properties(shin-context-fixtures PROPERTIES TIMEOUT 120)
    else()
        set_tests_properties(shin-context-fixtures PROPERTIES TIMEOUT 60)
    endif()
    add_test(
        NAME shin-context-index-faults
        COMMAND ${BASH_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_index_fault_tests.sh
                $<TARGET_FILE:shin-context>
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(shin-context-index-faults PROPERTIES TIMEOUT 60)
    add_test(
        NAME shin-context-operability
        COMMAND ${BASH_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_operability_tests.sh
                ${JQ_EXECUTABLE}
                $<TARGET_FILE:shin-context>
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(shin-context-operability PROPERTIES TIMEOUT 120)
    add_test(
        NAME shin-context-cold-start
        COMMAND ${BASH_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_cold_start_tests.sh
                $<TARGET_FILE:shin-context>
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(shin-context-cold-start PROPERTIES TIMEOUT 60)
    add_test(
        NAME shin-context-adoption-plan
        COMMAND ${BASH_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_adoption_plan_tests.sh
                ${JQ_EXECUTABLE}
                $<TARGET_FILE:shin-context>
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(shin-context-adoption-plan PROPERTIES TIMEOUT 60)
    add_test(NAME shin-context-adoption-guidance
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_adoption_guidance_retrieval.py
                $<TARGET_FILE:shin-context> ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(shin-context-adoption-guidance PROPERTIES TIMEOUT 30)
    add_test(
        NAME shin-context-codex-skill
        COMMAND ${BASH_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_skill_contract_tests.sh
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(shin-context-codex-skill PROPERTIES TIMEOUT 30)
    add_test(
        NAME shin-context-hermes-plugin
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_hermes_plugin_tests.py
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(
        shin-context-hermes-plugin
        PROPERTIES TIMEOUT 30 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    )
    add_test(
        NAME shin-context-adoption
        COMMAND ${BASH_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_adoption_tests.sh
                ${JQ_EXECUTABLE}
                $<TARGET_FILE:shin-context>
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(shin-context-adoption PROPERTIES TIMEOUT 60)
    add_test(
        NAME shin-context-profile-suggest
        COMMAND ${BASH_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_profile_suggest_tests.sh
                ${JQ_EXECUTABLE}
                $<TARGET_FILE:shin-context>
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(shin-context-profile-suggest PROPERTIES TIMEOUT 60)
    add_test(
        NAME shin-context-index-lifecycle
        COMMAND ${BASH_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_index_lifecycle_tests.sh
                ${JQ_EXECUTABLE}
                $<TARGET_FILE:shin-context>
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(shin-context-index-lifecycle PROPERTIES TIMEOUT 90)
    add_test(
        NAME shin-context-ready
        COMMAND ${BASH_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ready_tests.sh
                ${JQ_EXECUTABLE}
                $<TARGET_FILE:shin-context>
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(shin-context-ready PROPERTIES TIMEOUT 120)
    add_test(
        NAME shin-context-session-resume
        COMMAND ${BASH_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_session_resume_tests.sh
                ${JQ_EXECUTABLE}
                $<TARGET_FILE:shin-context>
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(shin-context-session-resume PROPERTIES TIMEOUT 120)
    add_test(
        NAME shin-context-universality
        COMMAND ${BASH_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_universality_tests.sh
                ${JQ_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_BINARY_DIR}/reports/cmake-universality.json
    )
    set_tests_properties(shin-context-universality PROPERTIES TIMEOUT 60)
endif()

if(SHIN_ENABLE_SANITIZERS)
    target_compile_options(shin-syntax-vendor PRIVATE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_compile_options(shin-context-core PUBLIC -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(shin-context-core INTERFACE -fsanitize=address,undefined)
    if(BUILD_TESTING AND SHIN_CONTEXT_BUILD_CLI)
        target_compile_options(shin-scanner-prototype-tests PRIVATE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(shin-scanner-prototype-tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(shin-execution-report-tests PRIVATE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(shin-execution-report-tests PRIVATE -fsanitize=address,undefined)
    endif()
endif()
