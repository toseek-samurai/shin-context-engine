#include "shin/observer/local_mcp_activity.h"
#include "shin/observer/local_history_files.h"
#include "shin/observer/local_project.h"
#include "shin/observer/local_service_platform.h"
#include "shin/observer/model_usage.h"
#include "local_session_analysis.h"
#include <sqlite3.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace shin::observer {
namespace {
using Clock = std::chrono::steady_clock;
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
constexpr std::int64_t retention_ms = 7LL * 24 * 3600 * 1000;
bool opaque(const Json& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}
void require(bool value) {
    if (!value) throw Error("mcp-history-unsafe", "MCP numerical activity history is unavailable.");
}
class SqlError : public Error {
public:
    explicit SqlError(int value) : Error("mcp-history-sql-failed", "MCP numerical activity history is unavailable."), sqlite_code(value) {}
    int sqlite_code;
};
void require_sql(sqlite3* db, bool value) { if (!value) throw SqlError(sqlite3_extended_errcode(db)); }
void sql(sqlite3* db, const char* text) { require_sql(db, sqlite3_exec(db, text, nullptr, nullptr, nullptr) == SQLITE_OK); }
Statement prepare(sqlite3* db, const char* text) {
    sqlite3_stmt* raw = nullptr;
    const int rc = sqlite3_prepare_v2(db, text, -1, &raw, nullptr);
    Statement result(raw, sqlite3_finalize); require_sql(db, rc == SQLITE_OK); return result;
}
std::int64_t scalar(sqlite3* db, const char* text) {
    auto stmt = prepare(db, text); require_sql(db, sqlite3_step(stmt.get()) == SQLITE_ROW);
    return sqlite3_column_int64(stmt.get(), 0);
}
std::string column(sqlite3_stmt* stmt, int index) {
    const auto* text = sqlite3_column_text(stmt, index);
    return text ? reinterpret_cast<const char*>(text) : "";
}
}

class LocalMcpActivity::Impl {
public:
    Impl(fs::path input, bool input_collect) : directory(std::move(input)), collect(input_collect),
        worker([this](std::stop_token stop) { run(stop); }) {}
    ~Impl() { worker.request_stop(); wake.notify_all(); worker.join(); }
    void append(const Json& input) {
        // No query, source, path, raw JSON-RPC ID, session text or error message.
        if (!input.is_object()) { std::lock_guard lock(mutex); ++dropped; return; }
        Json row;
        bool valid = input.is_object() && opaque(input.value("call_id", Json())) &&
            input.value("at_ms", Json()).is_number_integer() && input.at("at_ms").get<std::int64_t>() >= 0;
        for (const auto* key : {"project_id", "session_id", "service_request_id", "task_id", "parent_request_id", "generation_id", "read_file_id", "read_version_id"}) {
            const auto value = input.value(key, Json(""));
            valid = valid && (value == "" || opaque(value)); row[key] = value;
        }
        const auto method = input.value("method", Json());
        const auto outcome = input.value("outcome", Json());
        const auto written = input.value("write_state", Json());
        valid = valid && std::set<Json>{"query", "inspect", "read", "status", "projects", "help", "checkpoint", "resume", "unknown"}.contains(method) &&
            std::set<Json>{"unfinished", "success", "tool-error", "rpc-error"}.contains(outcome) &&
            std::set<Json>{"unobserved", "written", "failed"}.contains(written);
        const auto code = input.value("error_code", Json(""));
        valid = valid && code.is_string() && code.get_ref<const std::string&>().size() <= 96 &&
            code.get_ref<const std::string&>().find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos;
        const auto model = input.value("model_id", Json(""));
        valid = valid && model.is_string() && (model == "" || (opaque(row["session_id"]) && valid_reported_model(model.get<std::string>())));
        const auto duration = input.value("duration_ms", Json());
        valid = valid && (duration.is_null() || (duration.is_number() && duration.get<double>() >= 0 && duration.get<double>() <= 60000));
        std::lock_guard lock(mutex);
        if (!valid || failed || queue.size() >= 256) { ++dropped; return; }
        for (const auto* key : {"call_id", "at_ms", "method", "outcome", "write_state"}) row[key] = input.at(key);
        row["duration_ms"] = duration; row["error_code"] = code; row["model_id"] = model;
        row["purpose"] = input.value("purpose", Json()) == "diagnostic" ? "diagnostic" : "unclassified";
        const auto intent = input.value("intent", Json());
        row["intent"] = intent == "context-recovery" || intent == "explore" || intent == "follow-up" || intent == "checkpoint" ? intent : Json("unclassified");
        // Optional read labels are accepted only as a pair from a successful read.
        if (row["method"] != "read" || row["outcome"] != "success" ||
            !opaque(row["project_id"]) || !opaque(row["read_file_id"]) || !opaque(row["read_version_id"])) {
            row["read_file_id"] = ""; row["read_version_id"] = "";
        }
        queue.push_back(std::move(row)); wake.notify_all();
    }
    Json snapshot() const {
        std::lock_guard lock(mutex); auto result = cached;
        result["queued_updates_this_process"] = queue.size();
        result["dropped_updates_this_process"] = dropped;
        result["unconfirmed_updates_this_process"] = unconfirmed;
        return result;
    }
private:
    Json summarize(sqlite3* db) {
        Json result{{"schema_version", "shin.mcp-activity.v1"}, {"status", "ready"},
            {"scope", "retained-mcp-tool-calls"}, {"retention_days", 7}, {"row_limit", 10000},
            {"captured_at_ms", now_ms()}, {"calls", Json::object()}, {"outcomes", Json::object()},
            {"client_receipt", "not-observed"}, {"historical_backfill", false},
            {"service_executions_are_separate", true}, {"retry_pending", false}, {"model_attribution", "caller-reported-not-verified"}};
        auto meta = prepare(db, "SELECT coverage_start_ms,pruned_calls FROM meta WHERE singleton=1");
        require_sql(db, sqlite3_step(meta.get()) == SQLITE_ROW);
        result["coverage_start_ms"] = sqlite3_column_int64(meta.get(), 0);
        result["pruned_calls_since_creation"] = sqlite3_column_int64(meta.get(), 1);
        result["dropped_updates"] = scalar(db, "SELECT coalesce(sum(dropped),0) FROM losses");
        result["unconfirmed_updates"] = scalar(db, "SELECT coalesce(sum(unconfirmed),0) FROM losses");
        result["loss_scope"] = "retained-writer-reports";
        const auto now = now_ms();
        result["window_start_ms"] = std::max(result.at("coverage_start_ms").get<std::int64_t>(),now-retention_ms);
        if (result.at("pruned_calls_since_creation").get<std::int64_t>() > 0)
            result["window_start_ms"] = std::max(result.at("window_start_ms").get<std::int64_t>(),scalar(db,"SELECT coalesce(min(at_ms),0) FROM calls"));
        auto counts = prepare(db, "SELECT method,outcome,count(*),sum(write_state='written'),sum(write_state='failed'),"
            "sum(session_id=''),sum(model_id=''),sum(purpose='diagnostic') FROM calls "
            "WHERE at_ms>=?1 AND at_ms<=?2 GROUP BY method,outcome");
        sqlite3_bind_int64(counts.get(), 1, now - retention_ms); sqlite3_bind_int64(counts.get(), 2, now);
        std::uint64_t total = 0, written = 0, write_failed = 0, uncorrelated = 0, unknown_model = 0, diagnostic = 0;
        int rc;
        while ((rc = sqlite3_step(counts.get())) == SQLITE_ROW) {
            const auto method = column(counts.get(), 0), outcome = column(counts.get(), 1);
            const auto count = static_cast<std::uint64_t>(sqlite3_column_int64(counts.get(), 2));
            result["calls"][method] = result["calls"].value(method, std::uint64_t{0}) + count;
            result["outcomes"][outcome] = result["outcomes"].value(outcome, std::uint64_t{0}) + count;
            total += count; written += sqlite3_column_int64(counts.get(), 3); write_failed += sqlite3_column_int64(counts.get(), 4);
            uncorrelated += sqlite3_column_int64(counts.get(), 5); unknown_model += sqlite3_column_int64(counts.get(), 6);
            diagnostic += sqlite3_column_int64(counts.get(), 7);
        }
        require_sql(db, rc == SQLITE_DONE);
        result["attempts"] = total; result["written"] = written; result["write_failed"] = write_failed;
        result["context_calls"] = result["calls"].value("query", std::uint64_t{0}) +
            result["calls"].value("inspect", std::uint64_t{0}) + result["calls"].value("read", std::uint64_t{0}) + result["calls"].value("resume", std::uint64_t{0});
        result["uncorrelated"] = uncorrelated; result["model_unknown"] = unknown_model; result["diagnostic"] = diagnostic;
        result["future_calls"] = scalar(db, "SELECT count(*) FROM calls WHERE at_ms>CAST(strftime('%s','now') AS INTEGER)*1000+1000");
        auto sessions = prepare(db, "SELECT session_id,count(*),min(at_ms),max(at_ms),sum(method IN ('query','inspect','read','resume')),"
            "sum(outcome IN ('tool-error','rpc-error')),sum(intent='context-recovery'),count(distinct nullif(task_id,'')),"
            "sum(model_id='') FROM calls WHERE session_id<>'' AND at_ms>=?1 AND at_ms<=?2 "
            "GROUP BY session_id ORDER BY max(at_ms) DESC,session_id LIMIT 9");
        sqlite3_bind_int64(sessions.get(), 1, now-retention_ms); sqlite3_bind_int64(sessions.get(), 2, now);
        Json usage;
        if (collect) try { usage = read_model_usage(directory.parent_path()/"usage/state.json",now); }
        catch (const std::exception&) { usage = {{"status","unavailable"}}; }
        result["session_rows"] = Json::array();
        while ((rc = sqlite3_step(sessions.get())) == SQLITE_ROW) {
            const auto id = column(sessions.get(), 0);
            Json row{{"session_id", id}, {"timeline", Json::array()}, {"models", Json::object()}};
            int position = 1;
            for (const auto* key : {"calls", "first_at_ms", "last_at_ms", "context_calls", "errors", "recovery_requests", "tasks", "model_unknown"})
                row[key] = sqlite3_column_int64(sessions.get(), position++);
            auto recent = prepare(db, "SELECT at_ms,method,outcome,intent,task_id,model_id,duration_ms FROM calls "
                "WHERE session_id=?1 AND at_ms>=?2 AND at_ms<=?3 ORDER BY at_ms DESC,call_id LIMIT 8");
            sqlite3_bind_text(recent.get(), 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(recent.get(), 2, now-retention_ms); sqlite3_bind_int64(recent.get(), 3, now);
            int detail_rc;
            while ((detail_rc = sqlite3_step(recent.get())) == SQLITE_ROW)
                row["timeline"].push_back({{"at_ms", sqlite3_column_int64(recent.get(), 0)}, {"method", column(recent.get(), 1)},
                    {"outcome", column(recent.get(), 2)}, {"intent", column(recent.get(), 3)}, {"task_id", column(recent.get(), 4)},
                    {"model_id", column(recent.get(), 5)}, {"duration_ms", sqlite3_column_type(recent.get(), 6) == SQLITE_NULL ? Json(nullptr) : Json(sqlite3_column_double(recent.get(), 6))}});
            require_sql(db, detail_rc == SQLITE_DONE);
            auto queries = prepare(db, "SELECT at_ms FROM calls WHERE session_id=?1 AND method='query' AND outcome='success' AND at_ms>=?2 AND at_ms<=?3 ORDER BY at_ms DESC LIMIT 9");
            sqlite3_bind_text(queries.get(),1,id.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_int64(queries.get(),2,now-retention_ms); sqlite3_bind_int64(queries.get(),3,now);
            row["query_times"] = Json::array();
            while ((detail_rc = sqlite3_step(queries.get())) == SQLITE_ROW) row["query_times"].push_back(sqlite3_column_int64(queries.get(),0));
            require_sql(db,detail_rc == SQLITE_DONE);
            row["queries_complete"] = row["query_times"].size() <= 8;
            if (row["query_times"].size() > 8) row["query_times"].erase(8);
            auto models = prepare(db, "SELECT model_id,count(*) FROM calls WHERE session_id=?1 AND model_id<>'' AND at_ms>=?2 AND at_ms<=?3 "
                "GROUP BY model_id ORDER BY count(*) DESC,model_id LIMIT 4");
            sqlite3_bind_text(models.get(), 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(models.get(), 2, now-retention_ms); sqlite3_bind_int64(models.get(), 3, now);
            while ((detail_rc = sqlite3_step(models.get())) == SQLITE_ROW) row["models"][column(models.get(),0)] = sqlite3_column_int64(models.get(),1);
            require_sql(db, detail_rc == SQLITE_DONE);
            if (collect) {
                auto observations = prepare(db, "SELECT at_ms,method,outcome,write_state,purpose,project_id,read_file_id,read_version_id "
                    "FROM calls WHERE session_id=?1 AND at_ms>=?2 AND at_ms<=?3 ORDER BY at_ms,call_id LIMIT 10000");
                sqlite3_bind_text(observations.get(),1,id.c_str(),-1,SQLITE_TRANSIENT);
                sqlite3_bind_int64(observations.get(),2,now-retention_ms); sqlite3_bind_int64(observations.get(),3,now);
                Json calls = Json::array();
                while ((detail_rc = sqlite3_step(observations.get())) == SQLITE_ROW) {
                    Json call{{"at_ms",sqlite3_column_int64(observations.get(),0)}};
                    int index = 1;
                    for (const auto* key : {"method","outcome","write_state","purpose","project_id","read_file_id","read_version_id"})
                        call[key] = column(observations.get(),index++);
                    calls.push_back(std::move(call));
                }
                require_sql(db,detail_rc == SQLITE_DONE);
                row["analysis"] = analyze_session_activity(calls,usage,id,result.at("window_start_ms").get<std::int64_t>(),now,
                    result.at("dropped_updates") == 0 && result.at("unconfirmed_updates") == 0 && result.at("future_calls") == 0);
            }
            result["session_rows"].push_back(std::move(row));
        }
        require_sql(db, rc == SQLITE_DONE);
        result["sessions_truncated"] = result["session_rows"].size() > 8;
        if (result["session_rows"].size() > 8) result["session_rows"].erase(8);
        return result;
    }
    void run(std::stop_token stop) {
        std::optional<Clock::time_point> stop_deadline;
        std::unique_ptr<LocalHistoryFiles> files;
        bool checked = false;
        bool retry_pending = false;
        const auto writer = digest(std::to_string(service_process_id()) + ":" + std::to_string(Clock::now().time_since_epoch().count()));
        try {
            ensure_private_directory(directory);
            const auto identity = directory_identity(directory, true);
            for (;;) {
                if (stop.stop_requested() && !stop_deadline) stop_deadline = Clock::now() + std::chrono::seconds(2);
                try {
                    StateStore lease(directory, true, identity);
                    if (!files) files = std::make_unique<LocalHistoryFiles>(directory);
                    files->check();
                    sqlite3* raw = nullptr;
                    const int opened = sqlite3_open_v2(files->path().c_str(), &raw,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW, files->vfs());
                    Database db(raw, sqlite3_close); require_sql(raw, opened == SQLITE_OK);
                    require_sql(raw, sqlite3_db_config(raw, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr) == SQLITE_OK &&
                        sqlite3_db_config(raw, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) == SQLITE_OK);
                    sqlite3_limit(raw, SQLITE_LIMIT_LENGTH, 16384); sqlite3_busy_timeout(raw, 100);
                    const auto application = scalar(raw, "PRAGMA application_id");
                    if (application == 0 && files->created() && scalar(raw, "PRAGMA user_version") == 0) {
                        sql(raw, "PRAGMA page_size=4096; PRAGMA auto_vacuum=INCREMENTAL; BEGIN;"
                            "CREATE TABLE calls(call_id TEXT PRIMARY KEY,at_ms INTEGER NOT NULL,method TEXT NOT NULL,"
                            "outcome TEXT NOT NULL,write_state TEXT NOT NULL,project_id TEXT NOT NULL,session_id TEXT NOT NULL,"
                            "model_id TEXT NOT NULL,service_request_id TEXT NOT NULL,error_code TEXT NOT NULL,duration_ms REAL,purpose TEXT NOT NULL,"
                            "task_id TEXT NOT NULL DEFAULT '',intent TEXT NOT NULL DEFAULT 'unclassified',parent_request_id TEXT NOT NULL DEFAULT '',generation_id TEXT NOT NULL DEFAULT '');"
                            "CREATE INDEX calls_time ON calls(at_ms);"
                            "CREATE INDEX calls_session ON calls(session_id,at_ms);"
                            "CREATE TABLE meta(singleton INTEGER PRIMARY KEY CHECK(singleton=1),coverage_start_ms INTEGER NOT NULL,"
                            "pruned_calls INTEGER NOT NULL);"
                            "CREATE TABLE losses(writer TEXT PRIMARY KEY,at_ms INTEGER NOT NULL,dropped INTEGER NOT NULL,unconfirmed INTEGER NOT NULL);"
                            "PRAGMA application_id=1397246285; PRAGMA user_version=1;");
                        auto meta = prepare(raw, "INSERT INTO meta VALUES(1,?1,0)");
                        sqlite3_bind_int64(meta.get(), 1, now_ms()); require_sql(raw, sqlite3_step(meta.get()) == SQLITE_DONE);
                        sql(raw, "COMMIT");
                    }
                    require(scalar(raw, "PRAGMA application_id") == 1397246285 && scalar(raw, "PRAGMA user_version") == 1);
                    if (!checked) {
                        std::set<std::string> fields;
                        auto columns = prepare(raw,"PRAGMA table_info(calls)");
                        int column_rc;
                        while ((column_rc = sqlite3_step(columns.get())) == SQLITE_ROW) fields.insert(column(columns.get(),1));
                        require_sql(raw,column_rc == SQLITE_DONE);
                        columns.reset();
                        sql(raw,"BEGIN IMMEDIATE");
                        if (!fields.contains("read_file_id")) sql(raw,"ALTER TABLE calls ADD COLUMN read_file_id TEXT NOT NULL DEFAULT ''");
                        if (!fields.contains("read_version_id")) sql(raw,"ALTER TABLE calls ADD COLUMN read_version_id TEXT NOT NULL DEFAULT ''");
                        sql(raw,"COMMIT");
                        auto check = prepare(raw, "PRAGMA quick_check(1)");
                        require_sql(raw, sqlite3_step(check.get()) == SQLITE_ROW);
                        require(column(check.get(), 0) == "ok"); checked = true;
                    }
                    sql(raw, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA temp_store=MEMORY; PRAGMA cache_size=-512; PRAGMA max_page_count=8192;");
                    require(scalar(raw, "PRAGMA page_size") == 4096 && scalar(raw, "PRAGMA max_page_count") == 8192);
                    // Take one bounded batch. A failed/unknown commit is
                    // not replayed; each call ID is unique and terminal rows immutable.
                    std::deque<Json> batch;
                    std::uint64_t batch_drops, batch_unknown;
                    bool commit_pending = false;
                    { std::lock_guard lock(mutex); batch.swap(queue); batch_drops = dropped; batch_unknown = unconfirmed; }
                    try {
                        sql(raw, "BEGIN IMMEDIATE");
                        auto insert = prepare(raw, "INSERT INTO calls(call_id,at_ms,method,outcome,write_state,project_id,session_id,model_id,service_request_id,error_code,"
                            "duration_ms,purpose,task_id,intent,parent_request_id,generation_id,read_file_id,read_version_id) "
                            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18) "
                            "ON CONFLICT(call_id) DO UPDATE SET outcome=excluded.outcome,write_state=excluded.write_state,"
                            "project_id=excluded.project_id,session_id=excluded.session_id,model_id=excluded.model_id,"
                            "service_request_id=excluded.service_request_id,error_code=excluded.error_code,duration_ms=excluded.duration_ms,"
                            "purpose=excluded.purpose,task_id=excluded.task_id,intent=excluded.intent,"
                            "parent_request_id=excluded.parent_request_id,generation_id=excluded.generation_id,"
                            "read_file_id=excluded.read_file_id,read_version_id=excluded.read_version_id WHERE calls.outcome='unfinished'");
                        for (const auto& row : batch) {
                            int index = 1;
                            for (const auto* key : {"call_id", "at_ms", "method", "outcome", "write_state", "project_id", "session_id", "model_id", "service_request_id", "error_code", "duration_ms", "purpose", "task_id", "intent", "parent_request_id", "generation_id", "read_file_id", "read_version_id"}) {
                                const auto& value = row.at(key);
                                const int bound = value.is_string() ? sqlite3_bind_text(insert.get(), index, value.get_ref<const std::string&>().c_str(), -1, SQLITE_TRANSIENT) :
                                    value.is_null() ? sqlite3_bind_null(insert.get(), index) :
                                    value.is_number_integer() ? sqlite3_bind_int64(insert.get(), index, value.get<std::int64_t>()) :
                                    sqlite3_bind_double(insert.get(), index, value.get<double>());
                                require_sql(raw, bound == SQLITE_OK); ++index;
                            }
                            require_sql(raw, sqlite3_step(insert.get()) == SQLITE_DONE && sqlite3_reset(insert.get()) == SQLITE_OK);
                        }
                        auto age = prepare(raw, "DELETE FROM calls WHERE at_ms<?1");
                        sqlite3_bind_int64(age.get(), 1, now_ms() - retention_ms); require_sql(raw, sqlite3_step(age.get()) == SQLITE_DONE);
                        auto pruned = sqlite3_changes(raw);
                        sql(raw, "DELETE FROM calls WHERE call_id IN (SELECT call_id FROM calls ORDER BY at_ms DESC,call_id LIMIT -1 OFFSET 10000)");
                        pruned += sqlite3_changes(raw);
                        if (pruned) {
                            auto meta = prepare(raw, "UPDATE meta SET pruned_calls=pruned_calls+?1 WHERE singleton=1");
                            sqlite3_bind_int(meta.get(), 1, pruned);
                            require_sql(raw, sqlite3_step(meta.get()) == SQLITE_DONE);
                        }
                        if (batch_drops || batch_unknown) {
                            auto loss = prepare(raw, "INSERT INTO losses VALUES(?1,?2,?3,?4) ON CONFLICT(writer) DO UPDATE SET "
                                "at_ms=excluded.at_ms,dropped=max(dropped,excluded.dropped),unconfirmed=max(unconfirmed,excluded.unconfirmed)");
                            sqlite3_bind_text(loss.get(), 1, writer.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_int64(loss.get(), 2, now_ms());
                            sqlite3_bind_int64(loss.get(), 3, static_cast<sqlite3_int64>(batch_drops));
                            sqlite3_bind_int64(loss.get(), 4, static_cast<sqlite3_int64>(batch_unknown));
                            require_sql(raw, sqlite3_step(loss.get()) == SQLITE_DONE);
                        }
                        auto loss_age = prepare(raw, "DELETE FROM losses WHERE at_ms<?1");
                        sqlite3_bind_int64(loss_age.get(), 1, now_ms() - retention_ms); require_sql(raw, sqlite3_step(loss_age.get()) == SQLITE_DONE);
                        sql(raw, "DELETE FROM losses WHERE writer IN (SELECT writer FROM losses ORDER BY at_ms DESC,writer LIMIT -1 OFFSET 10000)");
                        commit_pending = true;
                        sql(raw, "COMMIT");
                    } catch (...) {
                        std::lock_guard lock(mutex);
                        if (commit_pending) unconfirmed += batch.size(); else dropped += batch.size();
                        throw;
                    }
                    sql(raw, "PRAGMA incremental_vacuum(32)");
                    auto summary = summarize(raw); files->check();
                    { std::lock_guard lock(mutex); cached = std::move(summary); retry_pending = false; }
                } catch (const Error& error) {
                    if (files) files->check(); // Never reacquire a substituted database.
                    const auto* failure = dynamic_cast<const SqlError*>(&error);
                    const int primary = failure ? failure->sqlite_code & 0xff : 0;
                    const bool transient = failure && (primary == SQLITE_BUSY || primary == SQLITE_LOCKED || primary == SQLITE_FULL ||
                        failure->sqlite_code == SQLITE_IOERR_WRITE || failure->sqlite_code == SQLITE_IOERR_FSYNC || failure->sqlite_code == SQLITE_IOERR_TRUNCATE);
                    if (error.code != "writer-busy" && !transient) throw;
                    std::lock_guard lock(mutex); cached["status"] = "unavailable"; cached["error_code"] = error.code;
                    retry_pending = true; cached["retry_pending"] = true;
                }
                std::unique_lock lock(mutex);
                if (stop.stop_requested() && ((!retry_pending && queue.empty()) || (stop_deadline && Clock::now() >= *stop_deadline))) {
                    dropped += queue.size(); queue.clear(); return;
                }
                if (stop.stop_requested()) wake.wait_for(lock, std::chrono::milliseconds(50));
                else if (retry_pending) wake.wait_for(lock, std::chrono::seconds(2), [&] { return stop.stop_requested(); });
                else if (queue.empty()) {
                    if (collect) wake.wait_for(lock, std::chrono::seconds(2), [&] { return stop.stop_requested() || !queue.empty(); });
                    else wake.wait(lock, [&] { return stop.stop_requested() || !queue.empty(); });
                }
                if (!queue.empty() && !stop.stop_requested())
                    wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return stop.stop_requested(); });
            }
        } catch (const std::exception&) {
            std::lock_guard lock(mutex); failed = true; dropped += queue.size(); queue.clear();
            cached["status"] = "unavailable"; cached["error_code"] = "mcp-history-unsafe";
            cached["retry_pending"] = false;
        }
    }
    fs::path directory;
    bool collect;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Json> queue;
    std::uint64_t dropped = 0, unconfirmed = 0;
    bool failed = false;
    Json cached{{"schema_version", "shin.mcp-activity.v1"}, {"status", "starting"}};
    std::jthread worker;
};
LocalMcpActivity::LocalMcpActivity(fs::path directory, bool collect) : impl(std::make_unique<Impl>(std::move(directory), collect)) {}
LocalMcpActivity::~LocalMcpActivity() = default;
void LocalMcpActivity::append(const Json& call) { impl->append(call); }
Json LocalMcpActivity::snapshot() const { return impl->snapshot(); }
} // namespace shin::observer
