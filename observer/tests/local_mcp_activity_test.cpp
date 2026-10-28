#include "shin/observer/local_mcp_activity.h"
#include "shin/observer/observer.h"
#include <sqlite3.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include "../../tests/core/windows_private_acl.h"
#endif
using namespace shin::observer;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void await(F predicate, const char* message = "MCP journal did not converge") {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (!predicate()) {
        check(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
struct Fixture {
    fs::path base = fs::canonical(fs::temp_directory_path()) / ("shin-mcp-activity-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { ensure_private_directory(base, true); }
    ~Fixture() { std::error_code ignored; fs::remove_all(base, ignored); }
};
}
int main() try {
    Fixture f;
    const auto directory = f.base / "journal";
    ensure_private_directory(directory);
    const auto at = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    Json call{{"call_id", std::string(64, 'a')}, {"at_ms", at}, {"method", "help"},
        {"outcome", "unfinished"}, {"write_state", "unobserved"}, {"query", "private-query-never-project"}};
    {
        auto lease = std::make_unique<StateStore>(directory, true);
        LocalMcpActivity journal(directory);
        for (int i = 0; i < 300; ++i) {
            call["call_id"] = digest(std::to_string(i)); journal.append(call);
        }
        check(journal.snapshot().at("dropped_updates_this_process").get<unsigned>() >= 44, "bounded queue must report gaps");
        lease.reset();
        await([&] { return journal.snapshot().value("attempts", 0) == 256; });
        const auto stored = journal.snapshot();
        check(stored.at("dropped_updates").get<unsigned>() >= 44, "gaps must persist");
        sqlite3* reader = nullptr;
        check(sqlite3_open((directory / "telemetry.sqlite").string().c_str(), &reader) == SQLITE_OK, "open contention fixture");
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> reader_guard(reader, sqlite3_close);
        check(sqlite3_exec(reader, "BEGIN; SELECT count(*) FROM calls", nullptr, nullptr, nullptr) == SQLITE_OK, "hold history reader");
        call["call_id"] = digest("blocked-commit"); call["outcome"] = "success"; call["write_state"] = "written";
        journal.append(call);
        await([&] { return journal.snapshot().value("unconfirmed_updates_this_process", 0) > 0; }, "commit contention was not observed");
        sqlite3_exec(reader, "ROLLBACK", nullptr, nullptr, nullptr); reader_guard.reset();
        // No second invocation is needed to resume after a transient commit error.
        await([&] { return journal.snapshot().value("status", "") == "ready" && journal.snapshot().value("unconfirmed_updates", 0) > 0; },
            "journal did not recover without another invocation after commit contention ended");
        call["call_id"] = digest("0"); call["outcome"] = "success"; call["write_state"] = "written";
        journal.append(call);
        await([&] { return journal.snapshot().value("written", 0) == 1; });
        call["outcome"] = "tool-error"; journal.append(call);
    }
    {
        LocalMcpActivity reopened(directory, true);
        await([&] { return reopened.snapshot().value("attempts", 0) == 256; });
        check(reopened.snapshot().at("outcomes").value("success", 0) == 1, "terminal duplicate changed its outcome");
    }
    // A previous prerelease v1 journal has no read labels. Expand atomically,
    // preserve its terminal rows, and leave the historical file identity unknown.
    const auto legacy_dir = f.base/"legacy";
    ensure_private_directory(legacy_dir);
    const auto legacy_path = legacy_dir/"telemetry.sqlite";
    sqlite3* old = nullptr;
    check(sqlite3_open(legacy_path.string().c_str(),&old) == SQLITE_OK,"create old journal");
    const std::string legacy = "CREATE TABLE calls(call_id TEXT PRIMARY KEY,at_ms INTEGER,method TEXT,outcome TEXT,write_state TEXT,"
        "project_id TEXT,session_id TEXT,model_id TEXT,service_request_id TEXT,error_code TEXT,duration_ms REAL,purpose TEXT,"
        "task_id TEXT,intent TEXT,parent_request_id TEXT,generation_id TEXT);"
        "CREATE TABLE meta(singleton INTEGER PRIMARY KEY,coverage_start_ms INTEGER,pruned_calls INTEGER);"
        "CREATE TABLE losses(writer TEXT PRIMARY KEY,at_ms INTEGER,dropped INTEGER,unconfirmed INTEGER);"
        "PRAGMA application_id=1397246285; PRAGMA user_version=1;"
        "INSERT INTO meta VALUES(1,"+std::to_string(at)+",0);"
        "INSERT INTO calls VALUES('"+digest("old-call")+"',"+std::to_string(at)+",'read','success','written','"+
        digest("project")+"','"+digest("chat")+"','','','',1,'unclassified','','unclassified','','');";
    check(sqlite3_exec(old,legacy.c_str(),nullptr,nullptr,nullptr) == SQLITE_OK,"seed old schema");
    sqlite3_close(old);
#ifdef _WIN32
    fixture_private_acl(legacy_path);
#else
    fs::permissions(legacy_path,fs::perms::owner_read|fs::perms::owner_write);
#endif
    {
        LocalMcpActivity upgraded(legacy_dir,true);
        await([&] { return upgraded.snapshot().value("attempts",0) == 1; });
        const auto analysis = upgraded.snapshot().at("session_rows")[0].at("analysis");
        check(analysis.at("reads").at("unknown_reads") == 1,"legacy read was guessed or deleted");
        Json read{{"call_id",digest("new-read")},{"session_id",digest("chat")},{"project_id",digest("project")},
            {"at_ms",at+1},{"method","read"},{"outcome","success"},{"write_state","written"},
            {"read_file_id",digest("file")},{"read_version_id",digest("version")}};
        upgraded.append(read);
        await([&] { return upgraded.snapshot().value("attempts",0) == 2; });
        check(upgraded.snapshot().at("session_rows")[0].at("analysis").at("reads").at("files") == 1,"new read label lost");
        read["call_id"] = digest("inspect-label"); read["method"] = "inspect"; upgraded.append(read);
        await([&] { return upgraded.snapshot().value("attempts",0) == 3; });
    }
    {
        ensure_private_directory(f.base/"usage");
        StateStore usage(f.base/"usage",true);
        Json markers = Json::array({
            {{"event_id",digest("turn-start")},{"session_id",digest("chat")},{"at_ms",at-1},{"kind","turn-start"}},
            {{"event_id",digest("turn-end")},{"session_id",digest("chat")},{"at_ms",at+2},{"kind","turn-finish"}}});
        usage.save({{"schema_version","shin.registered-usage.v1"},
            {"snapshot",{{"schema_version","shin.model-usage.v3"},{"config_sha256",digest("fixture-config")},
                {"updated_at_unix_ms",at+3},{"projects",Json::array({{{"id","p"},{"name","Fixture"},
                    {"workspace_keys",Json::array({digest("workspace")})}}})},{"sources",Json::object()},{"buckets",Json::array()}}},
            {"context_events",{{"schema_version","shin.context-events.v1"},{"source","codex"},{"events",markers},{"omitted",0},{"complete",true}}}});
    }
    {
        LocalMcpActivity reopened(legacy_dir,true);
        await([&] { return reopened.snapshot().value("attempts",0) == 3; });
        check(reopened.snapshot().at("session_rows")[0].at("analysis").at("reads").at("reads") == 2,"reopen changed read accounting");
        const auto episodes = reopened.snapshot().at("session_rows")[0].at("analysis").at("episodes");
        check(episodes.size() == 1 && episodes[0].at("reads").at("reads") == 2,"registered host events were not joined in the background");
        sqlite3* reader = nullptr;
        check(sqlite3_open(legacy_path.string().c_str(),&reader) == SQLITE_OK,"open upgraded fixture");
        sqlite3_stmt* stmt = nullptr;
        check(sqlite3_prepare_v2(reader,"SELECT count(*) FROM calls WHERE method='inspect' AND read_file_id<>''",-1,&stmt,nullptr) == SQLITE_OK &&
            sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt,0) == 0,"inspection incorrectly recorded a file read");
        sqlite3_finalize(stmt); sqlite3_close(reader);
    }
    const auto path = directory / "telemetry.sqlite";
    {
        std::ifstream file(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), {});
        check(bytes.find("private-query-never-project") == std::string::npos, "private payload entered journal");
    }
    sqlite3* db = nullptr;
    check(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK, "open fixture");
    const std::string seed = "BEGIN; WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<10005) "
        "INSERT INTO calls(call_id,at_ms,method,outcome,write_state,project_id,session_id,model_id,service_request_id,error_code,duration_ms,purpose) SELECT printf('%064x',x)," + std::to_string(at) + ", 'query','unfinished','unobserved','','','','','',NULL,'unclassified' FROM n;"
        "INSERT INTO calls(call_id,at_ms,method,outcome,write_state,project_id,session_id,model_id,service_request_id,error_code,duration_ms,purpose) VALUES('expired',0,'help','unfinished','unobserved','','','','','',NULL,'unclassified'); COMMIT;";
    check(sqlite3_exec(db, seed.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, "seed retained calls");
    sqlite3_close(db);
    {
        LocalMcpActivity bounded(directory, true);
        await([&] { return bounded.snapshot().value("attempts", 0) == 10000; });
        check(bounded.snapshot().at("pruned_calls_since_creation").get<unsigned>() >= 262, "age/cap pruning missing");
        std::unique_ptr<StateStore> guard;
        await([&] {
            try { guard = std::make_unique<StateStore>(directory, true); return true; }
            catch (const Error& error) { if (error.code != "writer-busy") throw; return false; }
        });
        fs::rename(path, directory / "preserved.sqlite");
        { std::ofstream replacement(path); }
        guard.reset();
        call["call_id"] = digest("after-replacement"); bounded.append(call);
        await([&] { return bounded.snapshot().value("error_code", "") == "mcp-history-unsafe"; });
        check(fs::file_size(path) == 0, "substituted file was modified");
    }
    std::cout << "MCP queue gaps, privacy, idempotency, retention and file identity passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
