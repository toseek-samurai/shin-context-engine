#include "shin/observer/local_history_files.h"
#include <sqlite3.h>
#include <chrono>
#include <iostream>
#ifdef _WIN32
#include "../../tests/core/windows_private_acl.h"
#include "shin/windows/file.h"
#include <cstring>
#include <latch>
#include <thread>
#else
#include <sys/stat.h>
#endif
using namespace shin::observer;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void sql(sqlite3* db, const char* command) {
    const int result = sqlite3_exec(db, command, nullptr, nullptr, nullptr);
    if (result != SQLITE_OK) throw std::runtime_error("SQLite code=" + std::to_string(result) + ": " + sqlite3_errmsg(db));
}
struct Fixture {
    fs::path root = fs::canonical(fs::temp_directory_path()) / ("shin-history-files-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { ensure_private_directory(root, true); }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};
#ifdef _WIN32
using NativeHandle = std::unique_ptr<void, decltype(&CloseHandle)>;
NativeHandle pending_delete(const fs::path& path) {
    NativeHandle handle(CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr), CloseHandle);
    check(handle.get() != INVALID_HANDLE_VALUE, "open deletion fixture");
    // Retain the classic delete-pending name until the last handle closes.
    FILE_DISPOSITION_INFO disposition{TRUE};
    check(SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition, sizeof(disposition)) != 0,
        "mark fixture for deletion");
    FILE_STANDARD_INFO info{};
    check(GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &info, sizeof(info)) && info.DeletePending,
        "fixture is not delete-pending");
    return handle;
}
void expect_unsafe(const LocalHistoryFiles& files) {
    try { files.check(); }
    catch (const Error& error) { check(error.code == "history-file-unsafe", "unexpected safety refusal"); return; }
    throw std::runtime_error("unsafe history was accepted");
}
int open_journal(const LocalHistoryFiles& files) {
    auto* vfs = sqlite3_vfs_find(files.vfs());
    check(vfs != nullptr, "private journal VFS missing");
    const auto path = files.path();
    std::unique_ptr<const char, decltype(&sqlite3_free_filename)> name(
        sqlite3_create_filename(path.c_str(), (path + "-journal").c_str(), (path + "-wal").c_str(), 0, nullptr),
        sqlite3_free_filename);
    check(name != nullptr, "allocate SQLite filename");
    const auto close = [](sqlite3_file* file) {
        if (file->pMethods) file->pMethods->xClose(file);
        sqlite3_free(file);
    };
    std::unique_ptr<sqlite3_file, decltype(close)> file(static_cast<sqlite3_file*>(sqlite3_malloc(vfs->szOsFile)), close);
    check(file != nullptr, "allocate journal handle");
    std::memset(file.get(), 0, static_cast<std::size_t>(vfs->szOsFile));
    int flags = 0;
    return vfs->xOpen(vfs, sqlite3_filename_journal(name.get()), file.get(),
        SQLITE_OPEN_MAIN_JOURNAL | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, &flags);
}
void deleted_journal_recovers() {
    for (const bool write : {false, true}) {
        Fixture fixture; LocalHistoryFiles files(fixture.root);
        sqlite3* raw = nullptr;
        const int opened = sqlite3_open_v2(files.path().c_str(), &raw, SQLITE_OPEN_READWRITE, files.vfs());
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        check(opened == SQLITE_OK, "open journal recovery fixture");
        sql(raw, "PRAGMA temp_store=MEMORY; CREATE TABLE proof(value); INSERT INTO proof VALUES(1);");
        const auto journal = fixture.root / "telemetry.sqlite-journal";
        shin::windows::Directory directory(fixture.root, shin::windows::Directory::Access::private_owner);
        check(directory.create_empty_file("telemetry.sqlite-journal"), "create journal deletion fixture");
        auto held = pending_delete(journal);
        std::latch started(1);
        std::jthread release([handle = std::move(held), &started]() mutable {
            started.count_down();
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            handle.reset();
        });
        started.wait();
        if (write) check(open_journal(files) == SQLITE_OK, "transient journal deletion blocked VFS open");
        else files.check();
        release.join();
        if (write) check(directory.remove("telemetry.sqlite-journal", 32U * 1024U * 1024U), "reopened journal was not private");
        check(!fs::exists(journal), "deleted journal was retained");
        files.check();
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(raw, "SELECT value FROM proof", -1, &statement, nullptr) == SQLITE_OK, "prepare recovery value");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> row(statement, sqlite3_finalize);
        check(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 1,
            "journal recovery changed committed data");
    }
}
void unsafe_journal_is_refused() {
    Fixture fixture; LocalHistoryFiles files(fixture.root);
    shin::windows::Directory directory(fixture.root, shin::windows::Directory::Access::private_owner);
    const auto journal = fixture.root / "telemetry.sqlite-journal";
    check(directory.create_empty_file("telemetry.sqlite-journal"), "create permanent pending journal");
    { auto held = pending_delete(journal); expect_unsafe(files);
        check(open_journal(files) == SQLITE_CANTOPEN, "permanently pending journal opened"); }
    check(directory.create_empty_file("telemetry.sqlite-journal"), "create unsafe ACL journal");
    fixture_private_acl(journal, true);
    expect_unsafe(files);
    check(open_journal(files) == SQLITE_CANTOPEN, "unsafe ACL journal opened");
}
void deleted_database_is_refused() {
    Fixture fixture; LocalHistoryFiles files(fixture.root);
    auto held = pending_delete(fixture.root / "telemetry.sqlite");
    expect_unsafe(files);
}
#endif
int main() {
    try {
#ifndef _WIN32
        ::umask(0077);
#else
        deleted_journal_recovers();
        unsafe_journal_is_refused();
        deleted_database_is_refused();
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
