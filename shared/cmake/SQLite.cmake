include_guard(GLOBAL)
if(WIN32)
    # Pinned amalgamation: no separately installed DLL/runtime or global PATH.
    include(FetchContent)
    FetchContent_Declare(shin_sqlite
        URL https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip
        URL_HASH SHA3_256=7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(shin_sqlite)
    add_library(shin-sqlite STATIC "${shin_sqlite_SOURCE_DIR}/sqlite3.c")
    target_include_directories(shin-sqlite SYSTEM PUBLIC "${shin_sqlite_SOURCE_DIR}")
    target_compile_definitions(shin-sqlite PRIVATE SQLITE_ENABLE_FTS5 SQLITE_THREADSAFE=1)
    add_library(SQLite::SQLite3 ALIAS shin-sqlite)
else()
    find_package(SQLite3 3.38 REQUIRED)
endif()
