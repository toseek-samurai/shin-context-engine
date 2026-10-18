#include "shin/observer/model_usage.h"
#include "shin/observer/observer.h"
#include <sqlite3.h>
#include <array>
#include <chrono>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace shin::observer {
namespace {
using Clock = std::chrono::steady_clock;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
constexpr std::uint64_t maximum_read_bytes = 4ULL * 1024 * 1024 * 1024;
const std::array<const char*, 5> fields{"input_tokens", "output_tokens", "cache_read_tokens", "cache_write_tokens", "reasoning_tokens"};
void need(bool valid, const char* code = "usage-source-unavailable") { if (!valid) throw Error(code, "Native usage source cannot be read safely."); }
std::int64_t integer(const Json& value) {
    need(value.is_number_integer(), "usage-counter-invalid"); const auto n = value.get<std::int64_t>(); need(n >= 0 && n <= 9007199254740991LL, "usage-counter-invalid"); return n;
}
std::int64_t stamp(const std::string& s) {
    need(s.size() >= 20 && s[4] == '-' && s[7] == '-' && s[10] == 'T' && s[13] == ':' && s[16] == ':' && s.back() == 'Z');
    const auto number = [&](std::size_t offset, std::size_t length) {
        for (std::size_t i = offset; i < offset + length; ++i) need(s[i] >= '0' && s[i] <= '9');
        return std::stoi(s.substr(offset, length));
    };
    const std::chrono::year_month_day day{std::chrono::year{number(0,4)}, std::chrono::month{static_cast<unsigned>(number(5,2))}, std::chrono::day{static_cast<unsigned>(number(8,2))}};
    const int hours = number(11,2), minutes = number(14,2), seconds = number(17,2);
    need(day.ok() && hours < 24 && minutes < 60 && seconds < 60);
    int fractional = 0;
    if (s.size() > 20) {
        need(s[19] == '.' && s.size() >= 22);
        for (std::size_t i = 20; i + 1 < s.size(); ++i) need(s[i] >= '0' && s[i] <= '9');
        for (std::size_t i = 0; i < 3; ++i) fractional = fractional * 10 + (20 + i + 1 < s.size() ? s[20 + i] - '0' : 0);
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::sys_days(day).time_since_epoch()).count() +
        ((static_cast<std::int64_t>(hours) * 60 + minutes) * 60 + seconds) * 1000 + fractional;
}
std::string day_text(std::int64_t ms) {
    const auto date = std::chrono::year_month_day(std::chrono::floor<std::chrono::days>(std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{ms}}));
    char text[16]; std::snprintf(text, sizeof(text), "%04d-%02u-%02u", static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day())); return text;
}
struct Database {
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db{nullptr, sqlite3_close};
    Clock::time_point deadline;
    std::stop_token stop;
    Database(const fs::path& path, Clock::time_point until, std::stop_token token) : deadline(until), stop(token) {
        need(fs::canonical(path) == fs::absolute(path).lexically_normal());
        struct stat status{}; need(::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) && status.st_uid == ::geteuid() && status.st_nlink == 1 && !(status.st_mode & 0022));
        sqlite3* raw = nullptr;
        const int rc = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW | SQLITE_OPEN_NOMUTEX, nullptr);
        db.reset(raw); need(rc == SQLITE_OK);
        sqlite3_busy_timeout(raw, 200);
        need(sqlite3_db_config(raw, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) == SQLITE_OK);
        sqlite3_progress_handler(raw, 1000, [](void* p) { auto* self = static_cast<Database*>(p); return self->stop.stop_requested() || Clock::now() > self->deadline ? 1 : 0; }, this);
    }
    Statement prepare(const char* sql) {
        sqlite3_stmt* raw = nullptr; const int rc = sqlite3_prepare_v2(db.get(), sql, -1, &raw, nullptr);
        Statement statement(raw, sqlite3_finalize); need(rc == SQLITE_OK); return statement;
    }
};
void record_skip_reason(Json& state, const std::string& reason) {
    auto& reasons = state["skipped_reasons"];
    // Retain the existing eight-bucket wire bound, including overflow.
    const auto bucket = !reasons.contains(reason) && reasons.size() >= 7 ? "rollout-other" : reason;
    reasons[bucket] = reasons.value(bucket,0) + 1;
}
std::string text(sqlite3_stmt* statement, int index) {
    const auto* p = sqlite3_column_text(statement, index); return p ? reinterpret_cast<const char*>(p) : "";
}
struct Rows {
    std::map<std::tuple<std::string,std::string,std::string,std::string>, Json> values;
    void add(const std::string& project, const std::string& client, const std::string& model, const std::string& measurement, std::int64_t when, const Json& usage) {
        const auto date = day_text(when), name = model.empty() ? "unknown" : model;
        const auto key = std::make_tuple(date, project, client, name);
        if (!values.contains(key)) {
            need(values.size() < 512);
            Json row{{"date",date},{"project",project},{"client",client},{"model",name},{"measurement",measurement},{"records",0}};
            for (const auto* field : fields) row[field] = 0;
            values[key] = std::move(row);
        }
        auto& row = values.at(key); need(row.at("measurement") == measurement);
        for (const auto* field : fields) {
            const auto old = integer(row.at(field)), n = integer(usage.at(field)); need(old <= 9007199254740991LL - n); row[field] = old + n;
        }
        row["records"] = integer(row.at("records")) + 1;
    }
};
struct Cached { std::uint64_t size = 0; std::int64_t mtime = 0; Rows rows; std::uint64_t records = 0; bool incomplete = false, future = false; };
std::map<std::string, Cached> cache;
std::mutex cache_mutex;

Cached codex_file(const fs::path& path, const Json& config, const std::string& session, const std::string& project,
    std::int64_t created, std::int64_t from, std::int64_t now, Clock::time_point deadline, std::uint64_t& bytes, std::stop_token stop) {
    const auto canonical = fs::canonical(path); need(canonical == path, "rollout-unsafe-file");
    const auto& source = config.at("sources").at("codex");
    bool allowed = false;
    for (const auto* key : {"sessions", "archives"}) {
        const fs::path configured = source.at(key).get<std::string>();
        if (!fs::exists(configured)) continue;
        const auto root = fs::canonical(configured);
        const auto relative = canonical.lexically_relative(root);
        if (!relative.empty() && *relative.begin() != "..") allowed = true;
    }
    need(allowed, "rollout-outside-roots");
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK); need(fd >= 0, "rollout-read-failed");
    struct Close { int fd; ~Close(){ ::close(fd); } } close{fd};
    struct stat s{}; need(::fstat(fd, &s) == 0, "rollout-read-failed");
    need(S_ISREG(s.st_mode) && s.st_uid == ::geteuid() && s.st_nlink == 1 && !(s.st_mode & 0022) && s.st_size >= 0, "rollout-unsafe-file");
    need(static_cast<std::uint64_t>(s.st_size) <= maximum_read_bytes, "rollout-too-large");
#ifdef __APPLE__
    const auto mtime = static_cast<std::int64_t>(s.st_mtimespec.tv_sec) * 1000000000 + s.st_mtimespec.tv_nsec;
#else
    const auto mtime = static_cast<std::int64_t>(s.st_mtim.tv_sec) * 1000000000 + s.st_mtim.tv_nsec;
#endif
    const auto key = path.string() + ":" + session + ":" + digest(config.dump()) + ":" + day_text(from);
    if (const auto it = cache.find(key); it != cache.end() && !it->second.future && it->second.size == static_cast<std::uint64_t>(s.st_size) && it->second.mtime == mtime) return it->second;
    // Admit a complete file against the existing sweep budget. Large sessions
    // need no separate smaller ceiling; memory remains a bounded line/chunk buffer.
    need(static_cast<std::uint64_t>(s.st_size) <= maximum_read_bytes - bytes, "usage-byte-budget-exhausted");
    Cached result; result.size = static_cast<std::uint64_t>(s.st_size); result.mtime = mtime;
    std::string line, model = "unknown", previous_total; bool discard = false, owned = false, seen_owner = false;
    std::array<char, 65536> buffer{};
    std::uint64_t consumed = 0, lines = 0;
    while (consumed < result.size) {
        need(!stop.stop_requested(), "usage-cancelled");
        need(Clock::now() < deadline, "usage-time-budget-exhausted");
        const auto count = ::read(fd, buffer.data(), std::min<std::uint64_t>(buffer.size(), result.size - consumed)); need(count > 0, "rollout-read-failed");
        consumed += static_cast<std::uint64_t>(count); bytes += static_cast<std::uint64_t>(count);
        for (ssize_t i = 0; i < count; ++i) {
            const char c = buffer[static_cast<std::size_t>(i)];
            if (c != '\n') {
                if (!discard) { if (line.size() < 1024*1024) line.push_back(c); else { discard = true; line.clear(); } }
                continue;
            }
            need(++lines <= 1000000, "rollout-line-limit");
            if (!discard && (line.find("\"token_count\"") != std::string::npos || line.find("\"turn_context\"") != std::string::npos || line.find("\"session_meta\"") != std::string::npos)) {
                const auto event = parse_json(line); const auto type = event.value("type", ""); const auto& payload = event.at("payload");
                if (type == "session_meta") {
                    owned = payload.value("id", "") == session;
                    need(owned || !seen_owner, "rollout-session-mismatch");
                    if (owned && !seen_owner) { seen_owner = true; previous_total.clear(); model = "unknown"; }
                }
                else if (owned && type == "turn_context") model = payload.value("model", "unknown");
                else if (owned && type == "event_msg" && payload.value("type", "") == "token_count" && payload.contains("info") && payload.at("info").is_object()) {
                    const auto& info = payload.at("info"); const auto total = info.at("total_token_usage").dump();
                    if (total != previous_total) {
                        const auto when = stamp(event.at("timestamp").get<std::string>());
                        if (when > now) result.future = true;
                        if (when >= from && when >= created && when <= now) {
                            const auto& last = info.at("last_token_usage");
                            Json usage{{"input_tokens", last.at("input_tokens")}, {"output_tokens", last.at("output_tokens")},
                                {"cache_read_tokens", last.value("cached_input_tokens", Json(0))}, {"cache_write_tokens", last.value("cache_write_input_tokens", Json(0))},
                                {"reasoning_tokens", last.value("reasoning_output_tokens", Json(0))}};
                            result.rows.add(project, "codex", model, "usage-event", when, usage); ++result.records;
                        }
                    }
                    previous_total = total;
                }
            } else if (discard) result.incomplete = true;
            line.clear(); discard = false;
        }
    }
    need(seen_owner, "rollout-session-mismatch");
    if (cache.size() >= 256) cache.clear();
    cache[key] = result;
    return result;
}
} // namespace

Json collect_model_usage(const Json& config, std::int64_t now, std::stop_token stop) {
    std::lock_guard lock(cache_mutex);
    need(config.at("schema_version") == "shin.usage-collector.v1" && config.at("projects").is_array() && !config.at("projects").empty() && config.at("projects").size() <= 24);
    std::map<std::string,std::string> paths; Json projects = Json::array(); std::set<std::string> ids;
    for (const auto& p : config.at("projects")) {
        const auto id = p.at("id").get<std::string>(); need(ids.insert(id).second && !id.empty());
        need(p.at("paths").is_array() && !p.at("paths").empty() && p.at("paths").size() <= 32);
        Json workspace_keys = Json::array();
        for (const auto& value : p.at("paths")) {
            const auto path = value.get<std::string>();
            need(fs::path(path).is_absolute() && fs::path(path).lexically_normal() == path && paths.emplace(path,id).second);
            workspace_keys.push_back(digest(path));
        }
        projects.push_back({{"id",id},{"name",p.at("name")},{"workspace_keys",workspace_keys}});
    }
    const auto from = (now / 86400000 - 29) * 86400000;
    const auto deadline = Clock::now() + std::chrono::seconds(120);
    Json output{{"schema_version","shin.model-usage.v3"},{"config_sha256",digest(config.dump())},{"updated_at_unix_ms",now},
        {"projects",projects},{"buckets",Json::array()},{"sources",Json::object()}};
    Rows rows; std::uint64_t bytes = 0;
    for (const auto* client : {"codex", "opencode", "hermes"}) {
        Json state{{"state","available"},{"attempted_at_unix_ms",now},{"last_success_unix_ms",now},{"from",day_text(from)},{"to",day_text(now)},
            {"source_records",0},{"unassigned_sessions",0},{"skipped_sessions",0},{"skipped_reasons",Json::object()},{"adapter_version","shin-native-v1"}};
        std::set<std::string> unassigned;
        try {
            Database db(config.at("sources").at(client).at("database").get<std::string>(), deadline, stop);
            if (std::string(client) == "codex") {
                auto sessions = db.prepare("SELECT id,rollout_path,cwd,coalesce(created_at_ms,created_at*1000) FROM threads WHERE coalesce(updated_at_ms,updated_at*1000)>=?1 ORDER BY updated_at DESC LIMIT 513");
                need(sqlite3_bind_int64(sessions.get(),1,from) == SQLITE_OK); int rc = 0, count = 0;
                while ((rc = sqlite3_step(sessions.get())) == SQLITE_ROW) {
                    need(++count <= 512); const auto session = text(sessions.get(),0), root = text(sessions.get(),2);
                    if (!paths.contains(root)) { unassigned.insert(session); continue; }
                    try {
                        const auto data = codex_file(text(sessions.get(),1),config,session,paths.at(root),sqlite3_column_int64(sessions.get(),3),from,now,deadline,bytes,stop);
                        for (const auto& [key, row] : data.rows.values) {
                            need(rows.values.contains(key) || rows.values.size() < 512);
                            auto& target = rows.values[key]; if (target.is_null()) target = row;
                            else { for (const auto* field : fields) target[field] = integer(target.at(field)) + integer(row.at(field)); target["records"] = integer(target.at("records")) + integer(row.at("records")); }
                        }
                        state["source_records"] = integer(state.at("source_records")) + static_cast<std::int64_t>(data.records);
                        if (data.incomplete) {
                            state["state"] = "partial";
                            record_skip_reason(state, "rollout-lines-omitted");
                        }
                    } catch (const std::exception& error) {
                        state["state"] = "partial"; state["skipped_sessions"] = integer(state.at("skipped_sessions")) + 1;
                        const auto* file = dynamic_cast<const fs::filesystem_error*>(&error);
                        const auto* known = dynamic_cast<const Error*>(&error);
                        const std::string reason = file ? (file->code() == std::errc::no_such_file_or_directory ? "rollout-missing" : "rollout-read-failed") :
                            known && known->code != "usage-source-unavailable" ? known->code : "rollout-format-unsupported";
                        record_skip_reason(state, reason);
                    }
                }
                need(rc == SQLITE_DONE);
            } else {
                const bool open = std::string(client) == "opencode";
                auto query = db.prepare(open ?
                    "SELECT s.id,s.directory,m.time_created,json_extract(m.data,'$.modelID'),json_extract(m.data,'$.tokens.input'),json_extract(m.data,'$.tokens.output'),json_extract(m.data,'$.tokens.cache.read'),json_extract(m.data,'$.tokens.cache.write'),json_extract(m.data,'$.tokens.reasoning') FROM message m JOIN session s ON m.session_id=s.id WHERE m.time_created>=?1 AND json_extract(m.data,'$.role')='assistant' AND json_extract(m.data,'$.time.completed') IS NOT NULL AND m.time_created>=s.time_created ORDER BY m.time_created LIMIT 200001" :
                    "SELECT s.id,s.cwd,CAST(coalesce(u.last_seen,s.last_activity_at,s.started_at)*1000 AS INTEGER),u.model,u.input_tokens,u.output_tokens,u.cache_read_tokens,u.cache_write_tokens,u.reasoning_tokens FROM session_model_usage u JOIN sessions s ON u.session_id=s.id WHERE coalesce(u.last_seen,s.last_activity_at,s.started_at)*1000>=?1 LIMIT 200001");
                need(sqlite3_bind_int64(query.get(),1,from) == SQLITE_OK); int rc = 0, count = 0;
                while ((rc = sqlite3_step(query.get())) == SQLITE_ROW) {
                    need(++count <= 200000 && Clock::now() < deadline && !stop.stop_requested());
                    const auto root = text(query.get(),1); if (!paths.contains(root)) { unassigned.insert(text(query.get(),0)); continue; }
                    const auto when = sqlite3_column_int64(query.get(),2); need(when >= from && when <= now);
                    Json usage = Json::object(); for (int i = 0; i < 5; ++i) {
                        need(sqlite3_column_type(query.get(),i+4) == SQLITE_INTEGER || sqlite3_column_type(query.get(),i+4) == SQLITE_NULL);
                        usage[fields[static_cast<std::size_t>(i)]] = sqlite3_column_int64(query.get(),i+4);
                    }
                    rows.add(paths.at(root),client,text(query.get(),3),open ? "usage-event" : "session-total",when,usage);
                    state["source_records"] = integer(state.at("source_records")) + 1;
                }
                need(rc == SQLITE_DONE);
            }
        } catch (...) { state["state"] = "unavailable"; state["last_success_unix_ms"] = nullptr; }
        state["unassigned_sessions"] = unassigned.size(); output["sources"][client] = std::move(state);
    }
    for (const auto& [key, row] : rows.values) { (void)key; output["buckets"].push_back(row); }
    need(!stop.stop_requested(), "usage-cancelled");
    (void)summarize_model_usage(output,now);
    return output;
}
} // namespace shin::observer
