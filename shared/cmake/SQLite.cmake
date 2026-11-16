include_guard(GLOBAL)
# One pinned runtime on every platform; no system SQLite or separately installed DLL.
include(FetchContent)
FetchContent_Declare(shin_sqlite
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
    URL_HASH SHA3_256=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(shin_sqlite)
add_library(shin-sqlite STATIC "${shin_sqlite_SOURCE_DIR}/sqlite3.c")
target_include_directories(shin-sqlite SYSTEM PUBLIC "${shin_sqlite_SOURCE_DIR}")
target_compile_definitions(shin-sqlite PRIVATE SQLITE_ENABLE_FTS5 SQLITE_THREADSAFE=1)
add_library(SQLite::SQLite3 ALIAS shin-sqlite)
