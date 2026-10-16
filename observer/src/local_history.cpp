#include "shin/observer/local_history.h"
#include "shin/observer/local_history_files.h"
#include "shin/observer/local_service_platform.h"
#include "shin/observer/observer.h"
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace shin::observer {
namespace {
constexpr std::int64_t retention_ms = 7LL * 24 * 3600 * 1000;
constexpr std::uintmax_t max_bytes = 32U * 1024U * 1024U;
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
void require(bool value, const char* code = "history-sql-failed") {
    if (!value) throw Error(code, "Local numerical history is unavailable.");
}
void execute(sqlite3* db, const char* sql) { require(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK); }
Statement prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
    Statement result(raw, sqlite3_finalize);
    require(rc == SQLITE_OK);
    return result;
}
void bind_text(sqlite3_stmt* stmt, int index, const std::string& text) {
    require(sqlite3_bind_text(stmt, index, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT) == SQLITE_OK);
}
std::int64_t scalar(sqlite3* db, const char* sql) {
    auto stmt = prepare(db, sql);
    require(sqlite3_step(stmt.get()) == SQLITE_ROW);
    return sqlite3_column_int64(stmt.get(), 0);
}
std::string column(sqlite3_stmt* stmt, int index) {
    const auto* value = sqlite3_column_text(stmt, index);
    return value ? reinterpret_cast<const char*>(value) : "";
}
} // namespace

class LocalHistory::Impl {
public:
    explicit Impl(fs::path input) : directory(std::move(input)), worker([this](std::stop_token stop) { run(stop); }) {}
    ~Impl() { worker.request_stop(); wake.notify_all(); worker.join(); }

    void append(const std::string& kind, const Json& input) {
        // Explicit projection: arbitrary caller payloads can never become history.
        Json row{{"kind", kind}, {"at_ms", now_ms()}};
        if (kind == "request" || kind == "admission") {
            for (const auto* key : {"project_id", "method", "outcome", "duration_ms", "response_bytes"}) row[key] = input.at(key);
            for (const auto* key : {"error_code", "reader_state", "purpose", "service_duration_ms",
                 "request_id", "session_id", "parent_request_id", "follow_up", "stage", "wait_ms", "admission_retries", "generation_id"})
                row[key] = input.at(key);
            for (const auto* key : {"admission", "documents", "source", "source_collection", "final_verification"})
                row[key] = input.at("phase_ms").at(key);
        } else if (kind == "event") {
            row["project_id"] = input.at("project_id");
            row["phase"] = input.at("phase");
            row["error_code"] = input.value("error_code", "");
        } else if (kind == "sample") {
            row["rss_bytes"] = input.at("rss_bytes");
            row["workers"] = input.at("workers");
        } else if (kind == "refresh") {
            row["project_id"] = input.at("project_id");
            row["outcome"] = input.at("outcome");
            row["phase"] = input.at("phase");
            row["duration_ms"] = input.at("duration_ms");
            row["active_ms"] = input.value("active_ms", Json(nullptr));
            row["sleep_ms"] = input.value("sleep_ms", Json(nullptr));
            row["error_code"] = input.at("error_code");
            row["stage"] = input.at("last_stage");
            row["refresh_phases"] = input.at("phases").dump();
            row["refresh_work"] = input.value("work", Json::object()).dump();
        } else return;
        for (const auto& item : row.items())
            if (item.value().is_string() && item.value().get_ref<const std::string&>().size() >
                (item.key() == "refresh_work" ? 2048U : item.key() == "refresh_phases" ? 512U : 96U)) return;
        std::lock_guard lock(mutex);
        if (failed || queue.size() >= 256) { ++dropped; return; }
        queue.push_back(std::move(row));
        wake.notify_one();
    }
    Json snapshot() const {
        std::lock_guard lock(mutex);
        auto result = cached;
        result["pending"] = queue.size() + in_flight + pending_summary;
        result["dropped_current_run"] = dropped;
        return result;
    }
private:
    Json summarize(sqlite3* db, std::int64_t now) {
        Json result{{"schema_version", "shin.local-history.v1"}, {"status", "ready"}, {"error_code", ""},
            {"retention_days", 7}, {"row_limit", 20000},
            {"row_limits", {{"request", 10000}, {"admission", 1000}, {"event", 4000}, {"sample", 3000},
                {"refresh_success", 1000}, {"refresh_error", 1000}, {"refresh_non_success", 1000},
                {"refresh_aggregate_rows", 4096}}},
            {"database_limit_bytes", max_bytes},
            {"captured_at_ms", now}, {"pruned_current_run", pruned}, {"timeline", Json::array()},
            {"projects", Json::array()}, {"events", Json::array()}, {"clock_basis", "utc-observed"}};
        auto extent = prepare(db, "SELECT count(*),min(at_ms),max(at_ms),sum(at_ms>?1) FROM events");
        require(sqlite3_bind_int64(extent.get(), 1, now) == SQLITE_OK && sqlite3_step(extent.get()) == SQLITE_ROW);
        result["rows"] = sqlite3_column_int64(extent.get(), 0);
        result["oldest_at_ms"] = sqlite3_column_type(extent.get(), 1) == SQLITE_NULL ? Json(nullptr) : Json(sqlite3_column_int64(extent.get(), 1));
        result["latest_at_ms"] = sqlite3_column_type(extent.get(), 2) == SQLITE_NULL ? Json(nullptr) : Json(sqlite3_column_int64(extent.get(), 2));
        result["future_rows"] = sqlite3_column_int64(extent.get(), 3);
        // Columnar hourly buckets keep the private RPC response bounded. Empty
        // hours are absent, never synthesized as successful zero-load periods.
        auto hours = prepare(db, "SELECT (at_ms/3600000)*3600000,"
            "sum(kind='request'),sum(kind='request' AND outcome='response-error'),sum(kind='request' AND outcome='unobserved'),"
            "sum(CASE WHEN method='query' THEN duration END),count(CASE WHEN method='query' THEN duration END),"
            "max(CASE WHEN method='query' THEN duration END),avg(rss),count(rss),max(workers) "
            "FROM events WHERE at_ms>=?1 AND at_ms<=?2 GROUP BY 1 ORDER BY 1 LIMIT 169");
        require(sqlite3_bind_int64(hours.get(), 1, now - retention_ms) == SQLITE_OK && sqlite3_bind_int64(hours.get(), 2, now) == SQLITE_OK);
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(hours.get())) == SQLITE_ROW) {
            auto row = Json::array();
            for (int i = 0; i < 10; ++i) row.push_back(sqlite3_column_type(hours.get(), i) == SQLITE_NULL ? Json(nullptr) : Json(sqlite3_column_double(hours.get(), i)));
            result["timeline"].push_back(std::move(row));
        }
        require(rc == SQLITE_DONE);
        auto coverage = prepare(db, "SELECT coverage_start_ms FROM refresh_rollup_meta WHERE singleton=1");
        require(sqlite3_step(coverage.get()) == SQLITE_ROW);
        result["refresh_aggregate_coverage"] = {{"scope", "since-aggregate-table-created"},
            {"coverage_start_ms", sqlite3_column_int64(coverage.get(), 0)}, {"historical_backfill", false}};
        result["refresh_aggregates"] = Json::array();
        const std::int64_t latest_hour = (now / 3600000) * 3600000;
        auto aggregates = prepare(db, "WITH selected AS ("
            "SELECT project,max(hour_ms) AS latest FROM refresh_hourly "
            "WHERE hour_ms>=?1 AND hour_ms<=?2 GROUP BY project ORDER BY latest DESC,project LIMIT 16) "
            "SELECT h.project,h.hour_ms,h.attempts,h.successes,h.errors,h.paused,"
            "CASE WHEN h.wall_samples=0 THEN NULL ELSE h.wall_sum END,"
            "CASE WHEN h.active_samples=0 THEN NULL ELSE h.active_sum END,"
            "CASE WHEN h.sleep_samples=0 THEN NULL ELSE h.sleep_sum END,h.active_max "
            "FROM refresh_hourly h JOIN selected s ON s.project=h.project "
            "WHERE h.hour_ms>=?1 AND h.hour_ms<=?2 ORDER BY h.project,h.hour_ms LIMIT 384");
        require(sqlite3_bind_int64(aggregates.get(), 1, latest_hour - 23LL * 3600000) == SQLITE_OK &&
            sqlite3_bind_int64(aggregates.get(), 2, latest_hour) == SQLITE_OK);
        while ((rc = sqlite3_step(aggregates.get())) == SQLITE_ROW) {
            Json row{{"project_id", column(aggregates.get(), 0)}, {"hour_ms", sqlite3_column_int64(aggregates.get(), 1)},
                {"attempts", sqlite3_column_int64(aggregates.get(), 2)}, {"successes", sqlite3_column_int64(aggregates.get(), 3)},
                {"errors", sqlite3_column_int64(aggregates.get(), 4)}, {"paused", sqlite3_column_int64(aggregates.get(), 5)}};
            for (const auto& [key, index] : std::array<std::pair<const char*, int>, 4>{{
                    {"wall_ms", 6}, {"active_ms", 7}, {"sleep_ms", 8}, {"active_max_ms", 9}}})
                row[key] = sqlite3_column_type(aggregates.get(), index) == SQLITE_NULL
                    ? Json(nullptr) : Json(sqlite3_column_double(aggregates.get(), index));
            result["refresh_aggregates"].push_back(std::move(row));
        }
        require(rc == SQLITE_DONE);
        result["refresh_diagnostics"] = Json::array();
        auto refreshes = prepare(db, "SELECT project,outcome,phase,duration,error_code,stage,refresh_phases,at_ms,active_duration,sleep_duration,refresh_work "
            "FROM events WHERE kind='refresh' ORDER BY id DESC LIMIT 32");
        while ((rc = sqlite3_step(refreshes.get())) == SQLITE_ROW) {
            Json phases = Json::object(), work = Json::object();
            try { const auto parsed = parse_json(column(refreshes.get(), 6)); phases = parsed.is_object() ? parsed : Json(nullptr); }
            catch (...) { phases = nullptr; }
            try { const auto parsed = parse_json(column(refreshes.get(), 10)); work = parsed.is_object() ? parsed : Json(nullptr); }
            catch (...) { work = nullptr; }
            result["refresh_diagnostics"].push_back({{"project_id", column(refreshes.get(), 0)},
                {"outcome", column(refreshes.get(), 1)}, {"phase", column(refreshes.get(), 2)},
                {"duration_ms", sqlite3_column_type(refreshes.get(), 3) == SQLITE_NULL ? Json(nullptr) : Json(sqlite3_column_double(refreshes.get(), 3))},
                {"active_ms", sqlite3_column_type(refreshes.get(), 8) == SQLITE_NULL ? Json(nullptr) : Json(sqlite3_column_double(refreshes.get(), 8))},
                {"sleep_ms", sqlite3_column_type(refreshes.get(), 9) == SQLITE_NULL ? Json(nullptr) : Json(sqlite3_column_double(refreshes.get(), 9))},
                {"error_code", column(refreshes.get(), 4)}, {"last_stage", column(refreshes.get(), 5)},
                {"phases", std::move(phases)}, {"work", std::move(work)}, {"at_ms", sqlite3_column_int64(refreshes.get(), 7)}});
        }
        require(rc == SQLITE_DONE);
        result["request_cohorts"] = Json::array();
        auto cohorts = prepare(db, "SELECT project,method,coalesce(purpose,'unclassified'),coalesce(reader_state,'unknown'),"
            "count(*),sum(outcome='response-error'),count(service_duration),avg(service_duration),max(service_duration),"
            "avg(duration),max(duration),avg(admission),avg(documents),avg(source),avg(source_collection),avg(final_verification),"
            "count(duration),count(admission),count(documents),count(source),count(source_collection),count(final_verification) "
            "FROM events WHERE kind='request' AND at_ms>=?1 AND at_ms<=?2 GROUP BY 1,2,3,4 ORDER BY count(*) DESC,1,2,3,4 LIMIT 49");
        require(sqlite3_bind_int64(cohorts.get(), 1, now - retention_ms) == SQLITE_OK && sqlite3_bind_int64(cohorts.get(), 2, now) == SQLITE_OK);
        while ((rc = sqlite3_step(cohorts.get())) == SQLITE_ROW) {
            auto row = Json::array();
            for (int i = 0; i < 4; ++i) row.push_back(column(cohorts.get(), i));
            for (int i = 4; i < 22; ++i)
                row.push_back(sqlite3_column_type(cohorts.get(), i) == SQLITE_NULL ? Json(nullptr) : Json(sqlite3_column_double(cohorts.get(), i)));
            result["request_cohorts"].push_back(std::move(row));
        }
        require(rc == SQLITE_DONE);
        result["request_cohorts_truncated"] = result["request_cohorts"].size() > 48;
        if (result["request_cohorts"].size() > 48) result["request_cohorts"].erase(48);
        auto minutes = prepare(db, "SELECT (at_ms/60000)*60000,"
            "sum(kind='request'),sum(kind='request' AND outcome='response-error'),sum(kind='request' AND outcome='unobserved'),"
            "sum(CASE WHEN method='query' THEN duration END),count(CASE WHEN method='query' THEN duration END),"
            "max(CASE WHEN method='query' THEN duration END),avg(rss),count(rss),max(workers),"
            "CASE WHEN count(DISTINCT run)=1 THEN min(run) END "
            "FROM events WHERE at_ms>=?1 AND at_ms<=?2 GROUP BY 1 ORDER BY 1 LIMIT 61");
        require(sqlite3_bind_int64(minutes.get(), 1, now - 3600000) == SQLITE_OK && sqlite3_bind_int64(minutes.get(), 2, now) == SQLITE_OK);
        result["minute_timeline"] = Json::array();
        while ((rc = sqlite3_step(minutes.get())) == SQLITE_ROW) {
            auto row = Json::array();
            for (int i = 0; i < 10; ++i) row.push_back(sqlite3_column_type(minutes.get(), i) == SQLITE_NULL ? Json(nullptr) : Json(sqlite3_column_double(minutes.get(), i)));
            row.push_back(sqlite3_column_type(minutes.get(), 10) == SQLITE_NULL ? Json(nullptr) : Json(column(minutes.get(), 10)));
            result["minute_timeline"].push_back(std::move(row));
        }
        require(rc == SQLITE_DONE);
        auto projects = prepare(db, "SELECT project,count(*),sum(outcome='response-error'),sum(outcome='unobserved'),avg(duration) "
            "FROM events WHERE kind='request' AND at_ms>=?1 AND at_ms<=?2 GROUP BY project ORDER BY count(*) DESC LIMIT 16");
        require(sqlite3_bind_int64(projects.get(), 1, now - retention_ms) == SQLITE_OK && sqlite3_bind_int64(projects.get(), 2, now) == SQLITE_OK);
        while ((rc = sqlite3_step(projects.get())) == SQLITE_ROW)
            result["projects"].push_back({{"id", column(projects.get(), 0)}, {"requests", sqlite3_column_int64(projects.get(), 1)},
                {"errors", sqlite3_column_int64(projects.get(), 2)}, {"unobserved", sqlite3_column_int64(projects.get(), 3)}});
        require(rc == SQLITE_DONE);
        auto events = prepare(db, "SELECT project,phase,at_ms,error_code FROM events WHERE kind='event' ORDER BY id DESC LIMIT 16");
        while ((rc = sqlite3_step(events.get())) == SQLITE_ROW)
            result["events"].push_back({{"project_id", column(events.get(), 0)}, {"phase", column(events.get(), 1)},
                {"at_ms", sqlite3_column_int64(events.get(), 2)}, {"error_code", column(events.get(), 3)}});
        require(rc == SQLITE_DONE);
        result["admission_rejections"] = scalar(db, "SELECT count(*) FROM events WHERE kind='admission'");
        result["admission_waits"] = scalar(db, "SELECT count(*) FROM events WHERE kind IN ('request','admission') AND wait_ms>0");
        result["wait_recoveries"] = scalar(db, "SELECT count(*) FROM events WHERE kind='request' AND wait_ms>0 AND outcome='response-ready'");
        result["observed_follow_ups"] = scalar(db, "SELECT count(*) FROM events WHERE kind='request' AND follow_up='observed'");
        result["correlated_requests"] = scalar(db, "SELECT count(*) FROM events WHERE kind='request' AND length(session_id)=64");
        result["admission_cohorts"] = Json::array();
        auto admissions = prepare(db, "SELECT project,method,error_code,count(*) FROM events WHERE kind='admission' "
            "GROUP BY 1,2,3 ORDER BY count(*) DESC,1,2,3 LIMIT 17");
        while ((rc = sqlite3_step(admissions.get())) == SQLITE_ROW)
            result["admission_cohorts"].push_back({column(admissions.get(), 0), column(admissions.get(), 1),
                column(admissions.get(), 2), sqlite3_column_int64(admissions.get(), 3)});
        require(rc == SQLITE_DONE);
        result["admission_cohorts_truncated"] = result["admission_cohorts"].size() > 16;
        if (result["admission_cohorts"].size() > 16) result["admission_cohorts"].erase(16);
        result["database_bytes"] = scalar(db, "PRAGMA page_count") * scalar(db, "PRAGMA page_size");
        return result;
    }
    void run(std::stop_token stop) {
        try {
            ensure_private_directory(directory);
            StateStore owner(directory, true);
            LocalHistoryFiles files(directory);
            const bool created = files.created();
            sqlite3* raw = nullptr;
            const int opened = sqlite3_open_v2(files.path().c_str(), &raw,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW, files.vfs());
            Database db(raw, sqlite3_close);
            require(opened == SQLITE_OK);
            require(sqlite3_db_config(raw, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr) == SQLITE_OK &&
                sqlite3_db_config(raw, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) == SQLITE_OK);
            sqlite3_limit(raw, SQLITE_LIMIT_LENGTH, 16384);
            sqlite3_busy_timeout(raw, 100);
            if (!created) {
                const auto version = scalar(raw, "PRAGMA user_version");
                require(scalar(raw, "PRAGMA application_id") == 1397246280 && (version == 1 || version == 2), "history-schema-unsupported");
                auto check = prepare(raw, "PRAGMA quick_check(1)");
                require(sqlite3_step(check.get()) == SQLITE_ROW && column(check.get(), 0) == "ok", "history-corrupt");
            } else {
                execute(raw, "PRAGMA page_size=4096; PRAGMA auto_vacuum=INCREMENTAL;"
                    "BEGIN; CREATE TABLE events(id INTEGER PRIMARY KEY,run TEXT NOT NULL,at_ms INTEGER NOT NULL,"
                    "kind TEXT NOT NULL,project TEXT,phase TEXT,method TEXT,outcome TEXT,duration REAL,bytes INTEGER,rss INTEGER,workers INTEGER);"
                    "CREATE INDEX events_time ON events(at_ms); PRAGMA application_id=1397246280; PRAGMA user_version=1; COMMIT;");
            }
            if (scalar(raw, "PRAGMA user_version") == 1) {
                execute(raw, "BEGIN IMMEDIATE; ALTER TABLE events ADD COLUMN service_duration REAL;"
                    "ALTER TABLE events ADD COLUMN reader_state TEXT; ALTER TABLE events ADD COLUMN purpose TEXT;"
                    "ALTER TABLE events ADD COLUMN error_code TEXT; ALTER TABLE events ADD COLUMN admission REAL;"
                    "ALTER TABLE events ADD COLUMN documents REAL; ALTER TABLE events ADD COLUMN source REAL;"
                    "ALTER TABLE events ADD COLUMN source_collection REAL; ALTER TABLE events ADD COLUMN final_verification REAL;"
                    "PRAGMA user_version=2; COMMIT;");
            }
            // Additive v2 extension: older v2 readers/writers ignore these nullable
            // columns. Do not rebuild the table or discard existing rows on upgrade.
            std::set<std::string> columns;
            { auto info = prepare(raw, "PRAGMA table_info(events)");
              while (sqlite3_step(info.get()) == SQLITE_ROW) columns.insert(column(info.get(), 1)); }
            execute(raw, "BEGIN IMMEDIATE");
            for (const auto* name : {"request_id", "session_id", "parent_request_id", "follow_up", "stage", "generation_id"})
                if (!columns.contains(name)) execute(raw, (std::string("ALTER TABLE events ADD COLUMN ") + name + " TEXT").c_str());
            if (!columns.contains("refresh_phases")) execute(raw, "ALTER TABLE events ADD COLUMN refresh_phases TEXT");
            if (!columns.contains("active_duration")) execute(raw, "ALTER TABLE events ADD COLUMN active_duration REAL");
            if (!columns.contains("sleep_duration")) execute(raw, "ALTER TABLE events ADD COLUMN sleep_duration REAL");
            if (!columns.contains("refresh_work")) execute(raw, "ALTER TABLE events ADD COLUMN refresh_work TEXT");
            for (const auto* name : {"wait_ms", "admission_retries"})
                if (!columns.contains(name)) execute(raw, (std::string("ALTER TABLE events ADD COLUMN ") + name + " REAL").c_str());
            execute(raw, "CREATE TABLE IF NOT EXISTS refresh_hourly("
                "project TEXT NOT NULL,hour_ms INTEGER NOT NULL,attempts INTEGER NOT NULL,successes INTEGER NOT NULL,"
                "errors INTEGER NOT NULL,paused INTEGER NOT NULL,wall_sum REAL NOT NULL,wall_samples INTEGER NOT NULL,"
                "active_sum REAL NOT NULL,active_samples INTEGER NOT NULL,sleep_sum REAL NOT NULL,sleep_samples INTEGER NOT NULL,"
                "active_max REAL,PRIMARY KEY(project,hour_ms));"
                "CREATE INDEX IF NOT EXISTS refresh_hourly_time ON refresh_hourly(hour_ms);");
            execute(raw, "CREATE TABLE IF NOT EXISTS refresh_rollup_meta(singleton INTEGER PRIMARY KEY CHECK(singleton=1),coverage_start_ms INTEGER NOT NULL)");
            auto coverage = prepare(raw, "INSERT OR IGNORE INTO refresh_rollup_meta(singleton,coverage_start_ms) VALUES(1,?1)");
            require(sqlite3_bind_int64(coverage.get(), 1, now_ms()) == SQLITE_OK && sqlite3_step(coverage.get()) == SQLITE_DONE);
            execute(raw, "COMMIT");
            execute(raw, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA temp_store=MEMORY; PRAGMA cache_size=-1024; PRAGMA max_page_count=8192;");
            require(scalar(raw, "PRAGMA page_size") == 4096 && scalar(raw, "PRAGMA max_page_count") == 8192, "history-size-policy");
            const auto run_id = digest(std::to_string(service_process_id()) + ":" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            for (;;) {
                files.check();
                std::deque<Json> batch;
                {
                    std::lock_guard lock(mutex);
                    batch.swap(queue);
                    in_flight = batch.size();
                }
                execute(raw, "BEGIN IMMEDIATE");
                auto insert = prepare(raw, "INSERT INTO events(run,at_ms,kind,project,phase,method,outcome,duration,bytes,rss,workers,"
                    "service_duration,admission,documents,source,source_collection,final_verification,reader_state,purpose,error_code,request_id,session_id,parent_request_id,follow_up,stage,wait_ms,admission_retries,generation_id,refresh_phases,active_duration,sleep_duration,refresh_work) "
                    "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32)");
                auto rollup = prepare(raw, "INSERT INTO refresh_hourly(project,hour_ms,attempts,successes,errors,paused,"
                    "wall_sum,wall_samples,active_sum,active_samples,sleep_sum,sleep_samples,active_max) "
                    "VALUES(?1,?2,1,?3,?4,?5,coalesce(?6,0),(?6 IS NOT NULL),coalesce(?7,0),(?7 IS NOT NULL),"
                    "coalesce(?8,0),(?8 IS NOT NULL),?7) ON CONFLICT(project,hour_ms) DO UPDATE SET "
                    "attempts=attempts+1,successes=successes+excluded.successes,errors=errors+excluded.errors,paused=paused+excluded.paused,"
                    "wall_sum=wall_sum+excluded.wall_sum,wall_samples=wall_samples+excluded.wall_samples,"
                    "active_sum=active_sum+excluded.active_sum,active_samples=active_samples+excluded.active_samples,"
                    "sleep_sum=sleep_sum+excluded.sleep_sum,sleep_samples=sleep_samples+excluded.sleep_samples,"
                    "active_max=CASE WHEN excluded.active_samples=0 THEN active_max WHEN active_max IS NULL OR excluded.active_max>active_max THEN excluded.active_max ELSE active_max END");
                for (const auto& row : batch) {
                    bind_text(insert.get(), 1, run_id);
                    require(sqlite3_bind_int64(insert.get(), 2, row.at("at_ms").get<std::int64_t>()) == SQLITE_OK);
                    int position = 3;
                    for (const auto* key : {"kind", "project_id", "phase", "method", "outcome"}) bind_text(insert.get(), position++, row.value(key, ""));
                    for (const auto* key : {"duration_ms", "response_bytes", "rss_bytes", "workers", "service_duration_ms",
                         "admission", "documents", "source", "source_collection", "final_verification"}) {
                        const auto value = row.value(key, Json(nullptr));
                        require((value.is_number() ? sqlite3_bind_double(insert.get(), position, value.get<double>()) : sqlite3_bind_null(insert.get(), position)) == SQLITE_OK);
                        ++position;
                    }
                    for (const auto* key : {"reader_state", "purpose", "error_code", "request_id", "session_id", "parent_request_id", "follow_up", "stage"})
                        bind_text(insert.get(), position++, row.value(key, ""));
                    for (const auto* key : {"wait_ms", "admission_retries"}) {
                        const auto value = row.value(key, Json());
                        require((value.is_number() ? sqlite3_bind_double(insert.get(), position, value.get<double>()) : sqlite3_bind_null(insert.get(), position)) == SQLITE_OK);
                        ++position;
                    }
                    bind_text(insert.get(), position, row.value("generation_id", ""));
                    bind_text(insert.get(), position + 1, row.value("refresh_phases", ""));
                    for (const auto* key : {"active_ms", "sleep_ms"}) {
                        const auto value = row.value(key, Json(nullptr));
                        require((value.is_number() ? sqlite3_bind_double(insert.get(), position + 2, value.get<double>()) : sqlite3_bind_null(insert.get(), position + 2)) == SQLITE_OK);
                        ++position;
                    }
                    bind_text(insert.get(), position + 2, row.value("refresh_work", "{}"));
                    require(sqlite3_step(insert.get()) == SQLITE_DONE && sqlite3_reset(insert.get()) == SQLITE_OK);
                    if (row.value("kind", "") == "refresh") {
                        bind_text(rollup.get(), 1, row.value("project_id", ""));
                        const auto at = row.at("at_ms").get<std::int64_t>();
                        require(sqlite3_bind_int64(rollup.get(), 2, (at / 3600000) * 3600000) == SQLITE_OK);
                        const auto outcome = row.value("outcome", "");
                        for (const int index : {3, 4, 5}) {
                            const bool value = (index == 3 && outcome == "success") ||
                                (index == 4 && outcome == "error") || (index == 5 && outcome == "paused");
                            require(sqlite3_bind_int(rollup.get(), index, value ? 1 : 0) == SQLITE_OK);
                        }
                        for (const auto& [index, key] : std::array<std::pair<int, const char*>, 3>{{
                                {6, "duration_ms"}, {7, "active_ms"}, {8, "sleep_ms"}}}) {
                            const auto value = row.value(key, Json(nullptr));
                            require((value.is_number() ? sqlite3_bind_double(rollup.get(), index, value.get<double>()) : sqlite3_bind_null(rollup.get(), index)) == SQLITE_OK);
                        }
                        require(sqlite3_step(rollup.get()) == SQLITE_DONE && sqlite3_reset(rollup.get()) == SQLITE_OK);
                    }
                }
                const auto now = now_ms();
                auto age = prepare(raw, "DELETE FROM events WHERE at_ms<?1");
                require(sqlite3_bind_int64(age.get(), 1, now - retention_ms) == SQLITE_OK && sqlite3_step(age.get()) == SQLITE_DONE);
                pruned += static_cast<std::uint64_t>(sqlite3_changes(raw));
                for (const auto& [kind, limit] : std::array<std::pair<const char*, int>, 4>{{
                     {"request", 10000}, {"admission", 1000}, {"event", 4000}, {"sample", 3000}}}) {
                    auto cap = prepare(raw, "DELETE FROM events WHERE id IN (SELECT id FROM events WHERE kind=?1 ORDER BY id DESC LIMIT -1 OFFSET ?2)");
                    bind_text(cap.get(), 1, kind);
                    require(sqlite3_bind_int(cap.get(), 2, limit) == SQLITE_OK && sqlite3_step(cap.get()) == SQLITE_DONE);
                    pruned += static_cast<std::uint64_t>(sqlite3_changes(raw));
                }
                for (const auto* sql : {
                        "DELETE FROM events WHERE id IN (SELECT id FROM events WHERE kind='refresh' AND outcome='success' ORDER BY id DESC LIMIT -1 OFFSET 1000)",
                        "DELETE FROM events WHERE id IN (SELECT id FROM events WHERE kind='refresh' AND outcome IN ('error','paused') ORDER BY id DESC LIMIT -1 OFFSET 1000)"}) {
                    auto cap = prepare(raw, sql);
                    require(sqlite3_step(cap.get()) == SQLITE_DONE);
                    pruned += static_cast<std::uint64_t>(sqlite3_changes(raw));
                }
                auto aggregate_age = prepare(raw, "DELETE FROM refresh_hourly WHERE hour_ms<?1");
                require(sqlite3_bind_int64(aggregate_age.get(), 1, now - retention_ms) == SQLITE_OK &&
                    sqlite3_step(aggregate_age.get()) == SQLITE_DONE);
                auto aggregate_cap = prepare(raw, "DELETE FROM refresh_hourly WHERE rowid IN ("
                    "SELECT rowid FROM refresh_hourly ORDER BY hour_ms DESC,project LIMIT -1 OFFSET 4096)");
                require(sqlite3_step(aggregate_cap.get()) == SQLITE_DONE);
                execute(raw, "COMMIT");
                {
                    std::lock_guard lock(mutex);
                    // Committed rows are not dropped if summary construction fails,
                    // but pending must stay nonzero until the cached view contains them.
                    pending_summary = in_flight;
                    in_flight = 0;
                }
                execute(raw, "PRAGMA incremental_vacuum(32)");
                auto summary = summarize(raw, now);
                files.check();
                {
                    std::unique_lock lock(mutex);
                    cached = std::move(summary);
                    in_flight = 0;
                    pending_summary = 0;
                    if (stop.stop_requested() && queue.empty()) break;
                    wake.wait_for(lock, std::chrono::seconds(2), [&] { return stop.stop_requested(); });
                }
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex);
            dropped += queue.size() + in_flight;
            queue.clear();
            in_flight = 0;
            pending_summary = 0;
            failed = true;
            cached["status"] = "unavailable";
            const auto* known = dynamic_cast<const Error*>(&error);
            cached["error_code"] = known ? known->code : "history-unavailable";
        }
    }
    fs::path directory;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Json> queue;
    std::size_t in_flight = 0, pending_summary = 0;
    std::uint64_t dropped = 0, pruned = 0;
    bool failed = false;
    Json cached{{"schema_version", "shin.local-history.v1"}, {"status", "starting"}, {"error_code", ""}};
    std::jthread worker;
};

LocalHistory::LocalHistory(fs::path directory) : impl(std::make_unique<Impl>(std::move(directory))) {}
LocalHistory::~LocalHistory() = default;
void LocalHistory::append(const std::string& kind, const Json& row) { impl->append(kind, row); }
Json LocalHistory::snapshot() const { return impl->snapshot(); }
} // namespace shin::observer
