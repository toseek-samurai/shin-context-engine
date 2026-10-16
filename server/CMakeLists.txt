cmake_minimum_required(VERSION 3.24)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SHIN_SERVER_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
if(NOT SHIN_SERVER_VERSION MATCHES "^((0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*))-(dev|rc)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "Server requires a canonical development or release-candidate version")
endif()
project(ShinServer VERSION "${CMAKE_MATCH_1}" LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(SHIN_SERVER_SANITIZERS "Instrument server code with ASan and UBSan" OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(PostgreSQL 13 REQUIRED)
include(FetchContent)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Oidc.cmake")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TEST OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(shin_pqxx
    URL https://codeload.github.com/jtv/libpqxx/tar.gz/refs/tags/8.0.2
    URL_HASH SHA256=028c5fba4982e759fe182af1103ccd08b02bb4e1f9f12a551e01d21cac0c4440
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
include("${CMAKE_CURRENT_SOURCE_DIR}/../shared/cmake/Http.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/HttpShutdown.cmake")
FetchContent_MakeAvailable(shin_pqxx)
# The static dependency is linked into Server; its SDK is not a runtime payload.
set_property(DIRECTORY "${shin_pqxx_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)

set(SHIN_CONTEXT_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(SHIN_ENABLE_SANITIZERS ${SHIN_SERVER_SANITIZERS} CACHE BOOL "" FORCE)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" SHIN_EMBEDDED_CORE_VERSION LIMIT_COUNT 1)
if(NOT SHIN_EMBEDDED_CORE_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-(dev|rc)\\.(0|[1-9][0-9]*))?$")
    message(FATAL_ERROR "Server requires a canonical release, development or release-candidate Core")
endif()
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_BINARY_DIR}/core")
set(SHIN_OBSERVER_PROTOCOL_SANITIZERS ${SHIN_SERVER_SANITIZERS})
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shared/observer-protocol" "${CMAKE_CURRENT_BINARY_DIR}/observer-protocol")

configure_file(cmake/version.h.in generated/shin/server/version.h @ONLY)
set(SHIN_SERVER_MIGRATION_035_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/035_device_sessions.sql")
file(READ "${SHIN_SERVER_MIGRATION_035_PATH}" SHIN_SERVER_MIGRATION_035_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_035_CHECKSUM "${SHIN_SERVER_MIGRATION_035_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_035_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/device_sessions_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char device_sessions_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_035_SQL@)SHIN_SQL";
inline constexpr char device_sessions_checksum[] = "@SHIN_SERVER_MIGRATION_035_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_034_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/034_external_identities.sql")
file(READ "${SHIN_SERVER_MIGRATION_034_PATH}" SHIN_SERVER_MIGRATION_034_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_034_CHECKSUM "${SHIN_SERVER_MIGRATION_034_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_034_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/external_identities_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char external_identities_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_034_SQL@)SHIN_SQL";
inline constexpr char external_identities_checksum[] = "@SHIN_SERVER_MIGRATION_034_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_033_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/033_observer_ownership.sql")
file(READ "${SHIN_SERVER_MIGRATION_033_PATH}" SHIN_SERVER_MIGRATION_033_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_033_CHECKSUM "${SHIN_SERVER_MIGRATION_033_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_033_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/observer_ownership_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char observer_ownership_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_033_SQL@)SHIN_SQL";
inline constexpr char observer_ownership_checksum[] = "@SHIN_SERVER_MIGRATION_033_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_032_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/032_journal_attribution.sql")
file(READ "${SHIN_SERVER_MIGRATION_032_PATH}" SHIN_SERVER_MIGRATION_032_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_032_CHECKSUM "${SHIN_SERVER_MIGRATION_032_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_032_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/journal_attribution_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char journal_attribution_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_032_SQL@)SHIN_SQL";
inline constexpr char journal_attribution_checksum[] = "@SHIN_SERVER_MIGRATION_032_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_031_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/031_route_history_security.sql")
file(READ "${SHIN_SERVER_MIGRATION_031_PATH}" SHIN_SERVER_MIGRATION_031_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_031_CHECKSUM "${SHIN_SERVER_MIGRATION_031_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_031_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/route_history_security_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char route_history_security_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_031_SQL@)SHIN_SQL";
inline constexpr char route_history_security_checksum[] = "@SHIN_SERVER_MIGRATION_031_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_030_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/030_preparation_row_security.sql")
file(READ "${SHIN_SERVER_MIGRATION_030_PATH}" SHIN_SERVER_MIGRATION_030_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_030_CHECKSUM "${SHIN_SERVER_MIGRATION_030_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_030_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/preparation_row_security_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char preparation_row_security_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_030_SQL@)SHIN_SQL";
inline constexpr char preparation_row_security_checksum[] = "@SHIN_SERVER_MIGRATION_030_CHECKSUM@";
}
]=] @ONLY)

set(SHIN_SERVER_MIGRATION_029_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/029_checkpoint_row_security.sql")
file(READ "${SHIN_SERVER_MIGRATION_029_PATH}" SHIN_SERVER_MIGRATION_029_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_029_CHECKSUM "${SHIN_SERVER_MIGRATION_029_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_029_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/checkpoint_row_security_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char checkpoint_row_security_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_029_SQL@)SHIN_SQL";
inline constexpr char checkpoint_row_security_checksum[] = "@SHIN_SERVER_MIGRATION_029_CHECKSUM@";
}
]=] @ONLY)

set(SHIN_SERVER_MIGRATION_028_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/028_preparation_scope_keys.sql")
file(READ "${SHIN_SERVER_MIGRATION_028_PATH}" SHIN_SERVER_MIGRATION_028_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_028_CHECKSUM "${SHIN_SERVER_MIGRATION_028_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_028_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/preparation_scope_keys_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char preparation_scope_keys_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_028_SQL@)SHIN_SQL";
inline constexpr char preparation_scope_keys_checksum[] = "@SHIN_SERVER_MIGRATION_028_CHECKSUM@";
}
]=] @ONLY)

set(SHIN_SERVER_MIGRATION_027_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/027_checkpoint_workspace_slots.sql")
file(READ "${SHIN_SERVER_MIGRATION_027_PATH}" SHIN_SERVER_MIGRATION_027_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_027_CHECKSUM "${SHIN_SERVER_MIGRATION_027_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_027_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/checkpoint_workspace_slots_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char checkpoint_workspace_slots_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_027_SQL@)SHIN_SQL";
inline constexpr char checkpoint_workspace_slots_checksum[] = "@SHIN_SERVER_MIGRATION_027_CHECKSUM@";
}
]=] @ONLY)

set(SHIN_SERVER_MIGRATION_026_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/026_workspace_request_keys.sql")
file(READ "${SHIN_SERVER_MIGRATION_026_PATH}" SHIN_SERVER_MIGRATION_026_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_026_CHECKSUM "${SHIN_SERVER_MIGRATION_026_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_026_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/workspace_request_keys_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char workspace_request_keys_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_026_SQL@)SHIN_SQL";
inline constexpr char workspace_request_keys_checksum[] = "@SHIN_SERVER_MIGRATION_026_CHECKSUM@";
}
]=] @ONLY)

set(SHIN_SERVER_MIGRATION_025_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/025_scoped_request_keys.sql")
file(READ "${SHIN_SERVER_MIGRATION_025_PATH}" SHIN_SERVER_MIGRATION_025_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_025_CHECKSUM "${SHIN_SERVER_MIGRATION_025_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_025_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/scoped_request_keys_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char scoped_request_keys_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_025_SQL@)SHIN_SQL";
inline constexpr char scoped_request_keys_checksum[] = "@SHIN_SERVER_MIGRATION_025_CHECKSUM@";
}
]=] @ONLY)

set(SHIN_SERVER_MIGRATION_024_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/024_personal_spaces.sql")
file(READ "${SHIN_SERVER_MIGRATION_024_PATH}" SHIN_SERVER_MIGRATION_024_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_024_CHECKSUM "${SHIN_SERVER_MIGRATION_024_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_024_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/personal_spaces_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char personal_spaces_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_024_SQL@)SHIN_SQL";
inline constexpr char personal_spaces_checksum[] = "@SHIN_SERVER_MIGRATION_024_CHECKSUM@";
}
]=] @ONLY)

set(SHIN_SERVER_MIGRATION_023_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/023_workspace_publication_history.sql")
file(READ "${SHIN_SERVER_MIGRATION_023_PATH}" SHIN_SERVER_MIGRATION_023_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_023_CHECKSUM "${SHIN_SERVER_MIGRATION_023_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_023_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/workspace_publication_history_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char workspace_publication_history_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_023_SQL@)SHIN_SQL";
inline constexpr char workspace_publication_history_checksum[] = "@SHIN_SERVER_MIGRATION_023_CHECKSUM@";
}
]=] @ONLY)

set(SHIN_SERVER_MIGRATION_022_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/022_preparation_workspace_scope.sql")
file(READ "${SHIN_SERVER_MIGRATION_022_PATH}" SHIN_SERVER_MIGRATION_022_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_022_CHECKSUM "${SHIN_SERVER_MIGRATION_022_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_022_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/preparation_workspace_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char preparation_workspace_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_022_SQL@)SHIN_SQL";
inline constexpr char preparation_workspace_checksum[] = "@SHIN_SERVER_MIGRATION_022_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_021_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/021_workspace_ownership.sql")
file(READ "${SHIN_SERVER_MIGRATION_021_PATH}" SHIN_SERVER_MIGRATION_021_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_021_CHECKSUM "${SHIN_SERVER_MIGRATION_021_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_021_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/workspace_ownership_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char workspace_ownership_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_021_SQL@)SHIN_SQL";
inline constexpr char workspace_ownership_checksum[] = "@SHIN_SERVER_MIGRATION_021_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_020_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/020_identity_directory.sql")
file(READ "${SHIN_SERVER_MIGRATION_020_PATH}" SHIN_SERVER_MIGRATION_020_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_020_CHECKSUM "${SHIN_SERVER_MIGRATION_020_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_020_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/identity_directory_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char identity_directory_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_020_SQL@)SHIN_SQL";
inline constexpr char identity_directory_checksum[] = "@SHIN_SERVER_MIGRATION_020_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_019_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/019_publication_history.sql")
file(READ "${SHIN_SERVER_MIGRATION_019_PATH}" SHIN_SERVER_MIGRATION_019_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_019_CHECKSUM "${SHIN_SERVER_MIGRATION_019_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_019_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/publication_history_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char publication_history_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_019_SQL@)SHIN_SQL";
inline constexpr char publication_history_checksum[] = "@SHIN_SERVER_MIGRATION_019_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_018_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/018_authenticated_receipts.sql")
file(READ "${SHIN_SERVER_MIGRATION_018_PATH}" SHIN_SERVER_MIGRATION_018_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_018_CHECKSUM "${SHIN_SERVER_MIGRATION_018_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_018_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/authenticated_receipts_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char authenticated_receipts_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_018_SQL@)SHIN_SQL";
inline constexpr char authenticated_receipts_checksum[] = "@SHIN_SERVER_MIGRATION_018_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_017_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/017_private_checkpoints.sql")
file(READ "${SHIN_SERVER_MIGRATION_017_PATH}" SHIN_SERVER_MIGRATION_017_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_017_CHECKSUM "${SHIN_SERVER_MIGRATION_017_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_017_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/private_checkpoints_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char private_checkpoints_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_017_SQL@)SHIN_SQL";
inline constexpr char private_checkpoints_checksum[] = "@SHIN_SERVER_MIGRATION_017_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_016_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/016_retrieval_journal_epochs.sql")
file(READ "${SHIN_SERVER_MIGRATION_016_PATH}" SHIN_SERVER_MIGRATION_016_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_016_CHECKSUM "${SHIN_SERVER_MIGRATION_016_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_016_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/retrieval_journal_epochs_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char journal_epochs_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_016_SQL@)SHIN_SQL";
inline constexpr char journal_epochs_checksum[] = "@SHIN_SERVER_MIGRATION_016_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_015_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/015_retrieval_journal_retention.sql")
file(READ "${SHIN_SERVER_MIGRATION_015_PATH}" SHIN_SERVER_MIGRATION_015_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_015_CHECKSUM "${SHIN_SERVER_MIGRATION_015_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_015_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/retrieval_journal_retention_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char journal_retention_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_015_SQL@)SHIN_SQL";
inline constexpr char journal_retention_checksum[] = "@SHIN_SERVER_MIGRATION_015_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_014_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/014_retrieval_journal.sql")
file(READ "${SHIN_SERVER_MIGRATION_014_PATH}" SHIN_SERVER_MIGRATION_014_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_014_CHECKSUM "${SHIN_SERVER_MIGRATION_014_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_014_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/retrieval_journal_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char retrieval_journal_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_014_SQL@)SHIN_SQL";
inline constexpr char retrieval_journal_checksum[] = "@SHIN_SERVER_MIGRATION_014_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_013_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/013_publication_withdrawals.sql")
file(READ "${SHIN_SERVER_MIGRATION_013_PATH}" SHIN_SERVER_MIGRATION_013_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_013_CHECKSUM "${SHIN_SERVER_MIGRATION_013_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_013_PATH}")
file(CONFIGURE OUTPUT generated/shin/server/publication_withdrawals_migration.h CONTENT [=[
#pragma once
namespace shin::server::generated_migrations {
inline constexpr char publication_withdrawals_sql[] = R"SHIN_SQL(@SHIN_SERVER_MIGRATION_013_SQL@)SHIN_SQL";
inline constexpr char publication_withdrawals_checksum[] = "@SHIN_SERVER_MIGRATION_013_CHECKSUM@";
}
]=] @ONLY)
set(SHIN_SERVER_MIGRATION_012_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/012_publication_proposals.sql")
file(READ "${SHIN_SERVER_MIGRATION_012_PATH}" SHIN_SERVER_MIGRATION_012_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_012_CHECKSUM "${SHIN_SERVER_MIGRATION_012_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_012_PATH}")
set(SHIN_SERVER_MIGRATION_011_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/011_observer_registry.sql")
file(READ "${SHIN_SERVER_MIGRATION_011_PATH}" SHIN_SERVER_MIGRATION_011_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_011_CHECKSUM "${SHIN_SERVER_MIGRATION_011_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_011_PATH}")
set(SHIN_SERVER_MIGRATION_010_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/010_server_recovery.sql")
file(READ "${SHIN_SERVER_MIGRATION_010_PATH}" SHIN_SERVER_MIGRATION_010_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_010_CHECKSUM "${SHIN_SERVER_MIGRATION_010_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_010_PATH}")
set(SHIN_SERVER_MIGRATION_009_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/009_audit_history.sql")
file(READ "${SHIN_SERVER_MIGRATION_009_PATH}" SHIN_SERVER_MIGRATION_009_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_009_CHECKSUM "${SHIN_SERVER_MIGRATION_009_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_009_PATH}")
set(SHIN_SERVER_MIGRATION_008_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/008_observer_history.sql")
file(READ "${SHIN_SERVER_MIGRATION_008_PATH}" SHIN_SERVER_MIGRATION_008_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_008_CHECKSUM "${SHIN_SERVER_MIGRATION_008_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_008_PATH}")
set(SHIN_SERVER_MIGRATION_007_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/007_observer_ingestion.sql")
file(READ "${SHIN_SERVER_MIGRATION_007_PATH}" SHIN_SERVER_MIGRATION_007_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_007_CHECKSUM "${SHIN_SERVER_MIGRATION_007_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_007_PATH}")
set(SHIN_SERVER_MIGRATION_006_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/006_workspaces.sql")
file(READ "${SHIN_SERVER_MIGRATION_006_PATH}" SHIN_SERVER_MIGRATION_006_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_006_CHECKSUM "${SHIN_SERVER_MIGRATION_006_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_006_PATH}")
set(SHIN_SERVER_MIGRATION_005_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/005_activation_audit.sql")
file(READ "${SHIN_SERVER_MIGRATION_005_PATH}" SHIN_SERVER_MIGRATION_005_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_005_CHECKSUM "${SHIN_SERVER_MIGRATION_005_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_005_PATH}")
set(SHIN_SERVER_MIGRATION_004_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/004_activation.sql")
file(READ "${SHIN_SERVER_MIGRATION_004_PATH}" SHIN_SERVER_MIGRATION_004_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_004_CHECKSUM "${SHIN_SERVER_MIGRATION_004_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_004_PATH}")
set(SHIN_SERVER_MIGRATION_003_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/003_retention.sql")
file(READ "${SHIN_SERVER_MIGRATION_003_PATH}" SHIN_SERVER_MIGRATION_003_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_003_CHECKSUM "${SHIN_SERVER_MIGRATION_003_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_003_PATH}")
set(SHIN_SERVER_MIGRATION_002_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/002_preparations.sql")
file(READ "${SHIN_SERVER_MIGRATION_002_PATH}" SHIN_SERVER_MIGRATION_002_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_002_CHECKSUM "${SHIN_SERVER_MIGRATION_002_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_002_PATH}")
set(SHIN_SERVER_MIGRATION_001_PATH "${CMAKE_CURRENT_SOURCE_DIR}/migrations/001_initial.sql")
file(READ "${SHIN_SERVER_MIGRATION_001_PATH}" SHIN_SERVER_MIGRATION_001_SQL)
string(SHA256 SHIN_SERVER_MIGRATION_001_CHECKSUM "${SHIN_SERVER_MIGRATION_001_SQL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHIN_SERVER_MIGRATION_001_PATH}")
configure_file(migrations/migrations_embedded.h.in generated/shin/server/migrations_embedded.h @ONLY)
add_library(shin-server-foundation STATIC src/config.cpp src/identity.cpp src/store.cpp
    src/identity_directory.cpp src/identity_directory_schema.cpp
    src/identity_external.cpp src/identity_external_schema.cpp
    src/identity_devices.cpp src/identity_device_schema.cpp
    src/workspace_ownership.cpp src/workspace_ownership_schema.cpp
    src/checkpoint_store.cpp src/checkpoint_model.cpp src/checkpoint_schema.cpp
    src/knowledge_scheduler.cpp src/session_bootstrap.cpp src/journal_writer.cpp
    src/retrieval_journal_model.cpp src/retrieval_journal_store.cpp src/retrieval_journal_schema.cpp src/retrieval_journal_retention.cpp
    src/retrieval_journal_lifecycle.cpp
    src/store_pool.cpp src/store_deadlines.cpp src/store_session.cpp src/store_schema.cpp src/schema_checks.cpp
    src/publication_store.cpp src/publication_resolution.cpp src/publication_schema.cpp src/publication_withdrawal.cpp
    src/publication_history.cpp src/retrieval_telemetry.cpp
    src/workspace_store.cpp src/workspace_schema.cpp src/preparation_store.cpp src/preparation_access.cpp src/preparation_access_schema.cpp
    src/route_access.cpp src/route_access_schema.cpp
    src/activation_store.cpp src/activation_schema.cpp src/observer_store.cpp
    src/observer_history_store.cpp src/observer_registry_store.cpp src/observer_schema.cpp
    src/audit_schema.cpp src/audit_store.cpp src/recovery_store.cpp)
target_include_directories(shin-server-foundation PUBLIC include "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(shin-server-foundation SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
target_link_libraries(shin-server-foundation PUBLIC libpqxx::pqxx OpenSSL::Crypto Threads::Threads shin-observer-protocol)
add_library(shin-server-retrieval STATIC src/snapshot_cache.cpp src/route_files.cpp src/route_catalog.cpp
    src/continuity_service.cpp src/continuity_evaluation.cpp
    src/active_snapshot.cpp src/knowledge_health.cpp src/knowledge_service.cpp
    src/retrieval_service.cpp src/preparation_service.cpp src/candidate_artifact.cpp src/preparation_inspection.cpp
    src/retention_files.cpp src/preparation_retention.cpp src/activation_service.cpp
    src/publication_service.cpp src/publication_review.cpp)
target_link_libraries(shin-server-retrieval PUBLIC shin-server-foundation shin-context-core)
add_executable(shin-server src/main.cpp src/http.cpp)
target_link_libraries(shin-server PRIVATE shin-server-retrieval shin-server-oidc httplib::httplib)
target_include_directories(shin-server SYSTEM BEFORE PRIVATE "${SHIN_SERVER_HTTP_DIR}")
target_compile_definitions(shin-server PRIVATE
    CPPHTTPLIB_HEADER_MAX_LENGTH=8192 CPPHTTPLIB_HEADER_MAX_COUNT=32
    CPPHTTPLIB_REQUEST_URI_MAX_LENGTH=2048)
foreach(target shin-server-foundation shin-server-retrieval shin-server)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    if(SHIN_SERVER_SANITIZERS)
        target_compile_options(${target} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC -fsanitize=address,undefined)
    endif()
endforeach()
include(CTest)
add_custom_target(shin-server-static-analysis
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/static_analysis.sh" "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS shin-server
    USES_TERMINAL VERBATIM)
if(BUILD_TESTING)
    add_executable(shin-server-external-identity-probe tests/external_identity_probe.cpp)
    add_executable(shin-server-device-probe tests/device_probe.cpp)
    target_link_libraries(shin-server-device-probe PRIVATE shin-server-foundation)
    target_compile_options(shin-server-device-probe PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    target_link_libraries(shin-server-external-identity-probe PRIVATE shin-server-foundation)
    target_compile_options(shin-server-external-identity-probe PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_executable(shin-server-oidc-tests tests/oidc_test.cpp)
    add_executable(shin-server-oidc-provider-probe tests/oidc_provider_probe.cpp)
    target_link_libraries(shin-server-oidc-provider-probe PRIVATE shin-server-oidc shin-server-foundation jwt-cpp::jwt-cpp)
    target_include_directories(shin-server-oidc-provider-probe SYSTEM PRIVATE "${shin_jwt_SOURCE_DIR}/include")
    target_compile_options(shin-server-oidc-provider-probe PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    target_link_libraries(shin-server-oidc-tests PRIVATE shin-server-oidc shin-server-foundation jwt-cpp::jwt-cpp)
    target_include_directories(shin-server-oidc-tests SYSTEM PRIVATE "${shin_jwt_SOURCE_DIR}/include")
    target_compile_options(shin-server-oidc-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-server-oidc COMMAND shin-server-oidc-tests)
    set_tests_properties(shin-server-oidc PROPERTIES TIMEOUT 30)
    add_executable(shin-server-oidc-probe tests/oidc_probe.cpp)
    target_link_libraries(shin-server-oidc-probe PRIVATE shin-server-oidc shin-server-foundation)
    target_compile_options(shin-server-oidc-probe PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_executable(shin-server-legacy-project-access-tests tests/legacy_project_access_test.cpp)
    target_include_directories(shin-server-legacy-project-access-tests PRIVATE src)
    target_link_libraries(shin-server-legacy-project-access-tests PRIVATE shin-server-foundation)
    target_compile_options(shin-server-legacy-project-access-tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
    add_test(NAME shin-server-legacy-project-access COMMAND shin-server-legacy-project-access-tests)
    add_executable(shin-server-identity-directory-tests tests/identity_directory_test.cpp)
    add_executable(shin-server-scoped-retrieval-probe tests/scoped_retrieval_probe.cpp)
    target_link_libraries(shin-server-scoped-retrieval-probe PRIVATE shin-server-retrieval)
    target_compile_options(shin-server-scoped-retrieval-probe PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_executable(shin-server-scoped-checkpoint-probe tests/scoped_checkpoint_probe.cpp)
    target_link_libraries(shin-server-scoped-checkpoint-probe PRIVATE shin-server-retrieval)
    target_compile_options(shin-server-scoped-checkpoint-probe PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_executable(shin-server-scoped-preparation-probe tests/scoped_preparation_probe.cpp tests/scoped_promotion_checks.cpp tests/scoped_request_key_checks.cpp tests/scoped_cleanup_checks.cpp tests/personal_route_checks.cpp)
    target_link_libraries(shin-server-scoped-preparation-probe PRIVATE shin-server-retrieval)
    target_compile_options(shin-server-scoped-preparation-probe PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    target_link_libraries(shin-server-identity-directory-tests PRIVATE shin-server-foundation)
    target_compile_options(shin-server-identity-directory-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-server-identity-directory COMMAND shin-server-identity-directory-tests)
    add_executable(shin-server-store-deadlines-tests tests/store_deadlines_test.cpp)
    target_include_directories(shin-server-store-deadlines-tests PRIVATE src)
    target_link_libraries(shin-server-store-deadlines-tests PRIVATE shin-server-foundation)
    target_compile_options(shin-server-store-deadlines-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-server-store-deadlines COMMAND shin-server-store-deadlines-tests)
    set_tests_properties(shin-server-store-deadlines PROPERTIES TIMEOUT 30)
    add_executable(shin-server-checkpoint-model-tests tests/checkpoint_model_test.cpp)
    target_link_libraries(shin-server-checkpoint-model-tests PRIVATE shin-server-foundation)
    target_compile_options(shin-server-checkpoint-model-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-server-checkpoint-model COMMAND shin-server-checkpoint-model-tests)
    add_executable(shin-server-journal-writer-tests tests/journal_writer_test.cpp)
    target_link_libraries(shin-server-journal-writer-tests PRIVATE shin-server-foundation)
    target_compile_options(shin-server-journal-writer-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-server-journal-writer COMMAND shin-server-journal-writer-tests)
    set_tests_properties(shin-server-journal-writer PROPERTIES TIMEOUT 45)
    add_executable(shin-server-session-bootstrap-tests tests/session_bootstrap_test.cpp)
    target_link_libraries(shin-server-session-bootstrap-tests PRIVATE shin-server-foundation)
    target_compile_options(shin-server-session-bootstrap-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-server-session-bootstrap COMMAND shin-server-session-bootstrap-tests)
    set_tests_properties(shin-server-session-bootstrap PROPERTIES TIMEOUT 30)

    add_executable(shin-server-knowledge-generation-tests tests/knowledge_generation_test.cpp)
    target_link_libraries(shin-server-knowledge-generation-tests PRIVATE shin-server-retrieval)
    target_compile_options(shin-server-knowledge-generation-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-server-knowledge-generation COMMAND shin-server-knowledge-generation-tests)
    add_executable(shin-server-knowledge-scheduler-tests tests/knowledge_scheduler_test.cpp)
    target_link_libraries(shin-server-knowledge-scheduler-tests PRIVATE shin-server-foundation)
    target_compile_options(shin-server-knowledge-scheduler-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-server-knowledge-scheduler COMMAND shin-server-knowledge-scheduler-tests)
    set_tests_properties(shin-server-knowledge-scheduler PROPERTIES TIMEOUT 30)
    add_executable(shin-server-json-codec-probe tests/json_codec_probe.cpp)
    target_link_libraries(shin-server-json-codec-probe PRIVATE shin-server-foundation)
    target_compile_options(shin-server-json-codec-probe PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_executable(shin-server-retrieval-journal-store-probe tests/retrieval_journal_store_probe.cpp)
    target_link_libraries(shin-server-retrieval-journal-store-probe PRIVATE shin-server-foundation)
    target_compile_options(shin-server-retrieval-journal-store-probe PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_executable(shin-server-retrieval-journal-tests tests/retrieval_journal_test.cpp)
    target_link_libraries(shin-server-retrieval-journal-tests PRIVATE shin-server-foundation)
    target_compile_options(shin-server-retrieval-journal-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-server-retrieval-journal COMMAND shin-server-retrieval-journal-tests)
    set_tests_properties(shin-server-retrieval-journal PROPERTIES TIMEOUT 30)
    add_executable(shin-server-retrieval-telemetry-tests tests/retrieval_telemetry_test.cpp)
    target_link_libraries(shin-server-retrieval-telemetry-tests PRIVATE shin-server-foundation)
    target_compile_options(shin-server-retrieval-telemetry-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-server-retrieval-telemetry COMMAND shin-server-retrieval-telemetry-tests)
    set_tests_properties(shin-server-retrieval-telemetry PROPERTIES TIMEOUT 30)
    add_executable(shin-server-retrieval-telemetry-benchmark tests/retrieval_telemetry_benchmark.cpp)
    target_link_libraries(shin-server-retrieval-telemetry-benchmark PRIVATE shin-server-foundation)
    target_compile_options(shin-server-retrieval-telemetry-benchmark PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_executable(shin-server-store-cost-probe tests/store_cost_probe.cpp)
    target_link_libraries(shin-server-store-cost-probe PRIVATE shin-server-foundation)
    target_compile_options(shin-server-store-cost-probe PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    add_test(NAME shin-server-oidc-provider
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/oidc_provider_test.py"
            --probe "$<TARGET_FILE:shin-server-oidc-provider-probe>")
    set_tests_properties(shin-server-oidc-provider PROPERTIES TIMEOUT 90)
    add_executable(shin-server-http-shutdown-tests tests/http_shutdown_test.cpp)
    target_link_libraries(shin-server-http-shutdown-tests PRIVATE httplib::httplib)
    target_include_directories(shin-server-http-shutdown-tests SYSTEM BEFORE PRIVATE "${SHIN_SERVER_HTTP_DIR}")
    if(SHIN_SERVER_SANITIZERS)
        target_compile_options(shin-server-http-shutdown-tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(shin-server-http-shutdown-tests PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME shin-server-http-shutdown COMMAND shin-server-http-shutdown-tests)
    set_tests_properties(shin-server-http-shutdown PROPERTIES TIMEOUT 10)
    add_test(NAME shin-server-native-json-codec
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_json_codec_test.py"
        "$<TARGET_FILE:shin-server-json-codec-probe>")
    set_tests_properties(shin-server-native-json-codec PROPERTIES TIMEOUT 30)
    add_test(NAME shin-server-store-cost-driver
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/store_cost_benchmark_test.py")
    set_tests_properties(shin-server-store-cost-driver PROPERTIES TIMEOUT 15)
    add_test(NAME shin-server-store-validation-driver
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/store_validation_checks_test.py")
    set_tests_properties(shin-server-store-validation-driver PROPERTIES TIMEOUT 15)
    add_test(NAME shin-server-publication-driver
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/publication_test_support.py")
    set_tests_properties(shin-server-publication-driver PROPERTIES TIMEOUT 15)
    add_test(NAME shin-server-contract-inventory
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract_inventory_test.py"
            --binary "$<TARGET_FILE:shin-server>")
    set_tests_properties(shin-server-contract-inventory PROPERTIES TIMEOUT 30)
    add_test(NAME shin-server-stand-guard
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/stand_guard_test.py")
    set_tests_properties(shin-server-stand-guard PROPERTIES TIMEOUT 15)
    add_test(NAME shin-server-preflight-driver
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/preparation_preflight_unit_test.py")
    set_tests_properties(shin-server-preflight-driver PROPERTIES TIMEOUT 15)
    foreach(SHIN_DRIVER_TEST dev_port headless_workflow_unit kit_restore_unit soak_integration_unit)
        add_test(NAME shin-server-${SHIN_DRIVER_TEST}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/${SHIN_DRIVER_TEST}_test.py")
        set_tests_properties(shin-server-${SHIN_DRIVER_TEST} PROPERTIES
            TIMEOUT 30 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    endforeach()
    add_executable(shin-server-config-tests tests/config_test.cpp)
    target_link_libraries(shin-server-config-tests PRIVATE shin-server-foundation)
    add_test(NAME shin-server-config COMMAND shin-server-config-tests)
    set_tests_properties(shin-server-config PROPERTIES TIMEOUT 20)
    add_executable(shin-server-cache-tests tests/snapshot_cache_test.cpp)
    target_link_libraries(shin-server-cache-tests PRIVATE shin-server-retrieval)
    add_test(NAME shin-server-cache COMMAND shin-server-cache-tests)
    set_tests_properties(shin-server-cache PROPERTIES TIMEOUT 30)
    add_executable(shin-server-candidate-artifact-tests tests/candidate_artifact_test.cpp)
    target_link_libraries(shin-server-candidate-artifact-tests PRIVATE shin-server-retrieval)
    add_test(NAME shin-server-candidate-artifact COMMAND shin-server-candidate-artifact-tests)
    set_tests_properties(shin-server-candidate-artifact PROPERTIES TIMEOUT 20)
    add_executable(shin-server-retention-files-tests tests/retention_files_test.cpp)
    target_link_libraries(shin-server-retention-files-tests PRIVATE shin-server-retrieval)
    add_test(NAME shin-server-retention-files COMMAND shin-server-retention-files-tests)
    set_tests_properties(shin-server-retention-files PROPERTIES TIMEOUT 20)
    add_executable(shin-server-profile-quality-tests ../tests/profile_quality_test.cpp)
    target_link_libraries(shin-server-profile-quality-tests PRIVATE shin-context-core)
    target_compile_options(shin-server-profile-quality-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_test(NAME shin-server-profile-quality COMMAND shin-server-profile-quality-tests ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(shin-server-profile-quality PROPERTIES TIMEOUT 60)
    add_executable(shin-server-preparation-store-tests tests/preparation_store_test.cpp)
    target_link_libraries(shin-server-preparation-store-tests PRIVATE shin-server-foundation)
    add_executable(shin-server-activation-store-tests tests/activation_store_test.cpp)
    target_link_libraries(shin-server-activation-store-tests PRIVATE shin-server-foundation)
    add_executable(shin-server-preflight-observation-tests
        tests/preflight_observation_test.cpp tests/preflight_inspection_override.cpp)
    target_link_libraries(shin-server-preflight-observation-tests PRIVATE shin-server-retrieval)
    target_compile_options(shin-server-preflight-observation-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    add_executable(shin-server-publication-review-tests tests/publication_review_test.cpp tests/publication_review_override.cpp)
    target_link_libraries(shin-server-publication-review-tests PRIVATE shin-server-retrieval)
    target_compile_options(shin-server-publication-review-tests PRIVATE -Wall -Wextra -Wpedantic -Wmissing-declarations)
    # This probe requires the dedicated PostgreSQL fixture; the HTTP harness owns its setup and teardown.
endif()

install(TARGETS shin-server RUNTIME DESTINATION bin)
install(FILES VERSION DESTINATION share/shin-server)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/../NOTICE"
    DESTINATION share/shin-server)
install(FILES "${shin_pqxx_SOURCE_DIR}/COPYING" DESTINATION share/shin-server/licenses/libpqxx)
install(FILES "${shin_httplib_SOURCE_DIR}/LICENSE" DESTINATION share/shin-server/licenses/cpp-httplib)
install(FILES "${shin_jwt_SOURCE_DIR}/LICENSE" DESTINATION share/shin-server/licenses/jwt-cpp)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nlohmann/LICENSE.MIT"
    DESTINATION share/shin-server/licenses/nlohmann)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/simdjson/LICENSE-APACHE-2.0"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/simdjson/LICENSE-MIT"
    DESTINATION share/shin-server/licenses/simdjson)
foreach(dependency tree-sitter tree-sitter-java tree-sitter-typescript tree-sitter-cpp)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/${dependency}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/${dependency}/UPSTREAM.json"
        DESTINATION "share/shin-server/licenses/${dependency}")
endforeach()
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/tree-sitter/src/unicode/LICENSE"
    DESTINATION share/shin-server/licenses/tree-sitter-unicode)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/pugixml/LICENSE.MIT"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/pugixml/UPSTREAM.json"
    DESTINATION share/shin-server/licenses/pugixml)
