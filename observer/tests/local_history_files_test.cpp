#include "shin/observer/local_history_files.h"
#include <sqlite3.h>
#include <chrono>
#include <iostream>
#ifndef _WIN32
#include <sys/stat.h>
#endif
using namespace shin::observer;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void sql(sqlite3* db, const char* command) { check(sqlite3_exec(db, command, nullptr, nullptr, nullptr) == SQLITE_OK, sqlite3_errmsg(db)); }
struct Fixture {
    fs::path root = fs::canonical(fs::temp_directory_path()) / ("shin-history-files-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { ensure_private_directory(root, true); }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};
int main() {
    try {
#ifndef _WIN32
        ::umask(0077);
#endif
        Fixture fixture;
        auto directory = fixture.root / fs::path(u8"\u0438\u0441\u0442\u043e\u0440\u0438\u044f");
        ensure_private_directory(directory, true);
        for (int i = 0; i < 4; ++i) {
            directory /= std::string(52, 'h') + std::to_string(i);
            ensure_private_directory(directory, true);
        }
        StateStore owner(directory, true);
        for (const bool fresh : {true, false}) {
            LocalHistoryFiles files(directory);
            check(files.created() == fresh, "history file creation/reopen differs");
            sqlite3* raw = nullptr;
            const auto opened = sqlite3_open_v2(files.path().c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, files.vfs());
            std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
            check(opened == SQLITE_OK, "private history open");
            sql(raw, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA temp_store=MEMORY;");
            if (fresh) sql(raw, "CREATE TABLE proof(value INTEGER); INSERT INTO proof VALUES(1);");
            sql(raw, "BEGIN IMMEDIATE; UPDATE proof SET value=2;");
            check(fs::exists(directory / "telemetry.sqlite-journal"), "transaction journal missing");
            files.check(); // Includes protected journal ACLs while it is still open.
            sql(raw, "ROLLBACK;");
            files.check();
            sqlite3_stmt* statement = nullptr;
            check(sqlite3_prepare_v2(raw, "SELECT value FROM proof", -1, &statement, nullptr) == SQLITE_OK, "prepare restored value");
            std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> row(statement, sqlite3_finalize);
            check(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 1, "rollback/reopen lost committed history");
        }
        std::cout << "private SQLite journal, rollback and Unicode reopen passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
