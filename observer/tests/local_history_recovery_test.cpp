#include "shin/observer/local_history.h"
#include "shin/observer/observer.h"
#include <sqlite3.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include "../../tests/core/windows_private_acl.h"
#else
#include <sys/stat.h>
#endif
using namespace shin::observer;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
struct Fixture {
    fs::path root = fs::canonical(fs::temp_directory_path()) / ("shin-history-recovery-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { ensure_private_directory(root, true); }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};
struct Connection {
    sqlite3* db = nullptr;
    explicit Connection(const fs::path& directory, const char* vfs = nullptr) {
        const auto text = (directory / "telemetry.sqlite").u8string();
        check(sqlite3_open_v2(reinterpret_cast<const char*>(text.c_str()), &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW, vfs) == SQLITE_OK, "open fixture history");
        sqlite3_busy_timeout(db, 1000);
    }
    ~Connection() { sqlite3_close(db); }
    void sql(const char* text) { check(sqlite3_exec(db, text, nullptr, nullptr, nullptr) == SQLITE_OK, sqlite3_errmsg(db)); }
    std::int64_t number(const char* text) {
        sqlite3_stmt* raw = nullptr;
        check(sqlite3_prepare_v2(db, text, -1, &raw, nullptr) == SQLITE_OK, "prepare fixture count");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
        check(sqlite3_step(raw) == SQLITE_ROW, "read fixture count");
        return sqlite3_column_int64(raw, 0);
    }
};
template<class Predicate> Json wait(LocalHistory& history, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    do {
        auto state = history.snapshot();
        if (predicate(state)) return state;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("history recovery wait: " + history.snapshot().dump());
}
Json ready(LocalHistory& history) {
    return wait(history, [](const auto& s) { return s.value("status", "") == "ready" && s.value("pending", 1U) == 0; });
}
void event(LocalHistory& history, const std::string& phase) {
    history.append("event", {{"project_id", "fixture"}, {"phase", phase}, {"error_code", ""}});
}
void seed(LocalHistory& history) {
    ready(history); event(history, "before");
    history.append("refresh", {{"project_id", "fixture"}, {"outcome", "success"}, {"phase", "current"},
        {"duration_ms", 42}, {"active_ms", 7}, {"sleep_ms", 0}, {"error_code", ""}, {"last_stage", "qualification"}, {"phases", Json::object()}});
    ready(history);
}
void verify(Connection& db, int events) {
    check(db.number("SELECT count(*) FROM events WHERE phase='before'") == 1, "committed baseline lost");
    check(db.number("SELECT attempts FROM refresh_hourly WHERE project='fixture'") == 1, "refresh aggregate duplicated/lost");
    check(db.number("SELECT count(*) FROM events") == events, "event replay or lost queued event");
    check(db.number("SELECT count(DISTINCT run) FROM events") == 1, "SQL retry changed the service-run identity");
}
void busy_recovery() {
    Fixture fixture; LocalHistory history(fixture.root); seed(history);
    Connection other(fixture.root); other.sql("BEGIN IMMEDIATE"); event(history, "failed-batch");
    auto failed = wait(history, [](const auto& s) { return s.value("retry_pending", false); });
    check((failed.value("sqlite_code", 0) & 255) == SQLITE_BUSY, "real writer lock did not cause SQLITE_BUSY");
    check(failed.at("dropped_current_run") == 1, "failed uncommitted batch not counted");
    event(history, "during-recovery"); other.sql("ROLLBACK");
    auto recovered = ready(history);
    check(recovered.at("recoveries") == 1 && !recovered.at("retry_pending"), "writer did not recover in place");
    check(recovered.at("unconfirmed_current_run") == 0, "BEGIN failure was falsely treated as ambiguous COMMIT");
    verify(other, 3);
}
void startup_busy_recovery() {
    Fixture fixture;
    { LocalHistory prior(fixture.root); seed(prior); }
    Connection other(fixture.root); other.sql("BEGIN EXCLUSIVE");
    LocalHistory history(fixture.root);
    wait(history, [](const auto& s) { return s.value("retry_pending", false); });
    event(history, "startup-queued"); other.sql("ROLLBACK"); ready(history);
    check(other.number("SELECT count(*) FROM events WHERE phase='before'") == 1 &&
        other.number("SELECT count(*) FROM events WHERE phase='startup-queued'") == 1 &&
        other.number("SELECT attempts FROM refresh_hourly WHERE project='fixture'") == 1,
        "startup retry lost or replayed existing history");
}
void commit_failure_is_not_replayed() {
    Fixture fixture; LocalHistory history(fixture.root); seed(history);
    Connection reader(fixture.root);
    reader.sql("BEGIN"); (void)reader.number("SELECT count(*) FROM events");
    history.append("refresh", {{"project_id", "fixture"}, {"outcome", "success"}, {"phase", "current"},
        {"duration_ms", 42}, {"error_code", ""}, {"last_stage", "qualification"}, {"phases", Json::object()}});
    auto failed = wait(history, [](const auto& s) { return s.value("retry_pending", false); });
    check(failed.at("unconfirmed_current_run") == 1 && failed.at("dropped_current_run") == 0,
        "COMMIT acknowledgement failure was not represented as unconfirmed");
    reader.sql("ROLLBACK"); event(history, "after-commit-error"); ready(history);
    verify(reader, 3);
}
void full_recovery() {
    Fixture fixture; LocalHistory history(fixture.root); seed(history);
#ifdef _WIN32
    // The pressure writer creates rollback journals too. Use the registered
    // private VFS so elevated runners cannot give those files a different owner.
    Connection other(fixture.root, "shin-private-history-win-v1");
#else
    Connection other(fixture.root);
#endif
    other.sql("PRAGMA max_page_count=8192; CREATE TABLE pressure(value BLOB)");
    // Fill only this disposable 32 MiB database, never the host filesystem.
    int result = SQLITE_OK;
    for (int batch = 0; batch < 100 && result == SQLITE_OK; ++batch) {
        other.sql("BEGIN IMMEDIATE");
        for (int i = 0; i < 64 && result == SQLITE_OK; ++i)
            result = sqlite3_exec(other.db, "INSERT INTO pressure VALUES(zeroblob(8192))", nullptr, nullptr, nullptr);
        if (result == SQLITE_OK) other.sql("COMMIT");
        else if (!sqlite3_get_autocommit(other.db)) other.sql("ROLLBACK");
    }
    check(result == SQLITE_FULL, "bounded fixture did not reach page limit");
    for (int i = 0; i < 64; ++i) {
        result = sqlite3_exec(other.db, "INSERT INTO pressure VALUES(zeroblob(8192))", nullptr, nullptr, nullptr);
        if (result == SQLITE_FULL) break;
        check(result == SQLITE_OK, "padding fixture failed unexpectedly");
    }
    for (int i = 0; i < 64; ++i) event(history, std::string(80, 'x') + std::to_string(i));
    auto failed = wait(history, [](const auto& s) { return s.value("retry_pending", false) && (s.value("sqlite_code", 0) & 255) == SQLITE_FULL; });
    check(failed.at("dropped_current_run").get<unsigned>() > 0, "failed full batch not observable");
    other.sql("DROP TABLE pressure"); event(history, "after-full");
    auto recovered = ready(history);
    check(recovered.at("recoveries").get<unsigned>() >= 1, "full history did not resume");
    verify(other, 3);
}
void replaced_during_retry() {
    Fixture fixture; LocalHistory history(fixture.root); seed(history);
    {
        Connection other(fixture.root); other.sql("BEGIN IMMEDIATE"); event(history, "blocked");
        wait(history, [](const auto& s) { return s.value("retry_pending", false); });
        other.sql("ROLLBACK");
    }
    const auto file = fixture.root / "telemetry.sqlite";
    const auto saved = fixture.root / "retained.sqlite";
    fs::rename(file, saved); fs::copy_file(saved, file);
#ifdef _WIN32
    fixture_private_acl(file);
#endif
    event(history, "must-not-write");
    auto refused = wait(history, [](const auto& s) { return s.value("error_code", "") == "history-file-changed"; });
    check(!refused.at("retry_pending"), "identity refusal kept retrying");
    Connection other(fixture.root); verify(other, 2);
}
void truncated_during_retry() {
    Fixture fixture; LocalHistory history(fixture.root); seed(history);
    {
        Connection other(fixture.root); other.sql("BEGIN IMMEDIATE"); event(history, "blocked");
        wait(history, [](const auto& s) { return s.value("retry_pending", false); });
        other.sql("ROLLBACK");
    }
    const auto file = fixture.root / "telemetry.sqlite";
    { std::ofstream truncated(file, std::ios::trunc); }
    auto refused = wait(history, [](const auto& s) { return s.value("error_code", "") == "history-schema-unsupported"; });
    check(!refused.at("retry_pending") && fs::file_size(file) == 0,
        "truncated previously initialized history was silently recreated");
}
void stop_during_retry() {
    Fixture fixture;
    auto history = std::make_unique<LocalHistory>(fixture.root); seed(*history);
    Connection other(fixture.root); other.sql("BEGIN IMMEDIATE"); event(*history, "blocked");
    wait(*history, [](const auto& s) { return s.value("retry_pending", false); });
    const auto started = std::chrono::steady_clock::now(); history.reset();
    check(std::chrono::steady_clock::now() - started < 1s, "shutdown waited through recovery backoff");
    other.sql("ROLLBACK");
}
void unsupported_existing_file() {
    Fixture fixture;
    const auto file = fixture.root / "telemetry.sqlite";
    { std::ofstream empty(file); }
#ifdef _WIN32
    fixture_private_acl(file);
#else
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
    LocalHistory history(fixture.root);
    auto refused = wait(history, [](const auto& s) { return s.value("error_code", "") == "history-schema-unsupported"; });
    check(!refused.at("retry_pending") && refused.at("recovery_attempts") == 0 && fs::file_size(file) == 0,
        "existing zero database was initialized or retried");
}
}
int main() try {
#ifndef _WIN32
    ::umask(0077);
#endif
    busy_recovery(); startup_busy_recovery(); commit_failure_is_not_replayed(); full_recovery(); replaced_during_retry(); truncated_during_retry(); stop_during_retry(); unsupported_existing_file();
    std::cout << "BUSY/FULL recovery, retained queue/rows/rollups, stable identity and hard refusals passed\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
