#include "shin/observer/model_usage.h"
#include "shin/observer/observer.h"
#include "shin/observer/local_registry.h"
#include <iostream>
#include <fstream>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

void source_fixture(std::int64_t now) {
    using namespace shin::observer;
    std::string name = "/tmp/shin-native-usage-XXXXXX";
    if (!::mkdtemp(name.data())) throw std::runtime_error("fixture-directory");
    const auto home = fs::canonical(name);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path,ec); } } cleanup{home};
    fs::create_directory(home / "sessions"); fs::create_directory(home / "archives");
    const auto log = home / "sessions/rollout.jsonl";
    Json meta{{"type","session_meta"},{"payload",{{"id","one"}}}};
    Json context{{"type","turn_context"},{"payload",{{"model","model-one"}}}};
    Json usage{{"input_tokens",100},{"output_tokens",20},{"cached_input_tokens",60},{"reasoning_output_tokens",5},{"total_tokens",120}};
    Json token{{"timestamp","2026-10-04T20:00:00.125Z"},{"type","event_msg"},{"payload",{{"type","token_count"},{"info",{{"total_token_usage",usage},{"last_token_usage",usage}}}}}};
    {
        auto inherited = meta; inherited["payload"]["id"] = "parent";
        std::ofstream out(log); out << inherited.dump() << '\n' << token.dump() << '\n' << meta.dump() << '\n' << context.dump() << '\n' << token.dump() << '\n' << token.dump() << '\n';
        out << Json{{"type","response_item"},{"payload",{{"text","DO-NOT-EXPORT"}}}}.dump() << '\n';
    }
    sqlite3* raw = nullptr;
    if (sqlite3_open((home / "state.sqlite").c_str(), &raw) != SQLITE_OK) throw std::runtime_error("fixture-sqlite");
    {
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw,sqlite3_close);
        if (sqlite3_exec(raw,"CREATE TABLE threads(id TEXT,rollout_path TEXT,cwd TEXT,created_at_ms INTEGER,created_at INTEGER,updated_at_ms INTEGER,updated_at INTEGER)",nullptr,nullptr,nullptr) != SQLITE_OK) throw std::runtime_error("fixture-schema");
        sqlite3_stmt* insert = nullptr;
        if (sqlite3_prepare_v2(raw,"INSERT INTO threads VALUES('one',?1,'/project',0,0,?2,0)",-1,&insert,nullptr) != SQLITE_OK) throw std::runtime_error("fixture-insert");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(insert,sqlite3_finalize);
        sqlite3_bind_text(insert,1,log.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(insert,2,now);
        if (sqlite3_step(insert) != SQLITE_DONE) throw std::runtime_error("fixture-row");
    }
    Json cfg{{"schema_version","shin.usage-collector.v1"},{"projects",Json::array({{{"id","p"},{"name","Project"},{"paths",Json::array({"/project"})}}})},
        {"sources",{{"codex",{{"database",(home/"state.sqlite").string()},{"sessions",(home/"sessions").string()},{"archives",(home/"archives").string()}}},
            {"opencode",{{"database",(home/"missing-open.sqlite").string()}}},{"hermes",{{"database",(home/"missing-hermes.sqlite").string()}}}}}};
    const auto first = collect_model_usage(cfg,now), second = collect_model_usage(cfg,now + 1);
    if (first.at("projects").at(0).at("workspace_keys") != Json::array({digest("/project")}))
        throw std::runtime_error("native-project-workspace-binding");
    if (first.at("buckets").size() != 1 || first.at("buckets").at(0).at("records") != 1 ||
        first.at("buckets") != second.at("buckets") || first.dump().find("DO-NOT-EXPORT") != std::string::npos ||
        first.at("sources").at("codex").at("state") != "available") throw std::runtime_error("native-dedup-or-privacy");
    const auto database_fixture = [&](const fs::path& path, const std::string& sql) {
        sqlite3* handle = nullptr;
        const int opened = sqlite3_open(path.c_str(), &handle);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(handle,sqlite3_close);
        if (opened != SQLITE_OK || sqlite3_exec(handle,sql.c_str(),nullptr,nullptr,nullptr) != SQLITE_OK)
            throw std::runtime_error("client-fixture-sqlite");
    };
    const auto millis = std::to_string(now - 1000), seconds = std::to_string(now / 1000 - 1);
    database_fixture(home / "open.sqlite",
        "CREATE TABLE session(id TEXT,directory TEXT,time_created INTEGER);"
        "CREATE TABLE message(session_id TEXT,time_created INTEGER,data TEXT);"
        "INSERT INTO session VALUES('open','/project',1);"
        "INSERT INTO message VALUES('open'," + millis + ","
        "'{\"role\":\"assistant\",\"modelID\":\"open-model\",\"time\":{\"completed\":1},\"tokens\":{\"input\":70,\"output\":10,\"cache\":{\"read\":30,\"write\":2},\"reasoning\":4}}');"
        "INSERT INTO message VALUES('open',0,'{\"role\":\"assistant\",\"time\":{\"completed\":1}}');"
        "INSERT INTO message VALUES('open'," + millis + ",' {\"role\":\"assistant\",\"text\":\"DO-NOT-EXPORT\"}');");
    database_fixture(home / "hermes.sqlite",
        "CREATE TABLE sessions(id TEXT,cwd TEXT,last_activity_at INTEGER,started_at INTEGER);"
        "CREATE TABLE session_model_usage(session_id TEXT,last_seen INTEGER,model TEXT,input_tokens INTEGER,output_tokens INTEGER,cache_read_tokens INTEGER,cache_write_tokens INTEGER,reasoning_tokens INTEGER);"
        "INSERT INTO sessions VALUES('hermes','/project'," + seconds + "," + seconds + ");"
        "INSERT INTO session_model_usage VALUES('hermes'," + seconds + ",'hermes-model',40,8,9,3,2);");
    cfg["sources"]["opencode"]["database"] = (home / "open.sqlite").string();
    cfg["sources"]["hermes"]["database"] = (home / "hermes.sqlite").string();
    const auto complete = collect_model_usage(cfg,now);
    const auto summary = summarize_model_usage(complete,now);
    auto malformed_events = complete;
    malformed_events["context_events"] = {{"schema_version","shin.context-events.v1"},{"source","codex"},
        {"complete",true},{"omitted",0},{"events",Json::array({{{"event_id","raw-session"}}})}};
    const auto isolated = summarize_model_usage(malformed_events,now);
    if (isolated.at("context_events").at("status") != "invalid" || isolated.at("projects") != summary.at("projects"))
        throw std::runtime_error("context-metadata-must-not-invalidate-token-snapshot");
    const auto& group = summary.at("projects").at(0);
    if (!summary.at("complete").get<bool>() || complete.at("buckets").size() != 3 ||
        group.at("totals").at("input_tokens") != 210 || group.at("totals").at("records") != 3 ||
        group.at("days").size() != 1 || group.at("days").at(0).at("input_tokens") != 170 ||
        complete.dump().find("DO-NOT-EXPORT") != std::string::npos)
        throw std::runtime_error("native-client-counter-semantics");
}

using namespace shin::observer;
void registered_fixture() {
    using namespace shin::observer;
    std::string name = "/tmp/shin-registered-usage-XXXXXX";
    if (!::mkdtemp(name.data())) throw std::runtime_error("fixture-directory");
    const auto home = fs::canonical(name);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path,ec); } } cleanup{home};
    ensure_local_registry(home);
    const auto registry = [&](const Json& roots) {
        Json projects = Json::array();
        for (const auto& root : roots) projects.push_back({{"root",root},{"root_identity","fixture-root"},{"workspace_identity","fixture-workspace"}});
        StateStore store(home/"registry",true);
        store.save({{"schema_version","shin.local-registry.v1"},{"home_identity",directory_identity(home,true)},{"projects",projects}});
    };
    registry(Json::array({"/fixture/one"}));
    Json sources = {{"codex",{{"database",(home/"absent.sqlite").string()}}},
                    {"hermes",{{"database",(home/"absent.sqlite").string()}}},
                    {"opencode",{{"database",(home/"absent.sqlite").string()}}}};
    Json seed{{"schema_version","shin.usage-collector.v1"},{"sources",sources},
              {"projects",Json::array({{{"id","custom"},{"name","Existing group"},{"paths",Json::array({"/fixture/one","/fixture/legacy"})}}})}};
    (void)collect_registered_model_usage(home,sources,seed);
    registry(Json::array({"/fixture/one","/fixture/two"}));
    (void)collect_registered_model_usage(home,sources);
    const auto original = read_text(home/"usage/state.json",kMaximumStateBytes,true);
    const auto result = parse_json(original);
    if (result.at("config").at("projects").at(0) != seed.at("projects").at(0) || result.at("snapshot").at("projects").size() != 2)
        throw std::runtime_error("automatic-groups-not-preserved");
    (void)read_model_usage(home/"usage/state.json",now_ms());
    std::stop_source cancel; cancel.request_stop();
    bool cancelled = false;
    try { (void)collect_registered_model_usage(home,sources,nullptr,cancel.get_token()); }
    catch (const Error& error) { cancelled = error.code == "usage-cancelled"; }
    if (!cancelled || read_text(home/"usage/state.json",kMaximumStateBytes,true) != original)
        throw std::runtime_error("cancelled-collection-mutated-history");
    bool refused = false;
    try { (void)collect_registered_model_usage(home,sources,seed); }
    catch (const Error& error) { refused = error.code == "usage-already-configured"; }
    if (!refused) throw std::runtime_error("existing-usage-overwritten");
}

int main() {
    ::umask(0077); // Fixture databases/logs are deliberately trusted private inputs.
    try {
        const std::int64_t now = 1791153600000LL;
        Json row{{"date", "2026-10-04"}, {"project", "project-alpha"}, {"client", "codex"}, {"model", "model-x"}, {"measurement", "usage-event"},
            {"input_tokens", 100}, {"output_tokens", 20}, {"cache_read_tokens", 200}, {"cache_write_tokens", 0},
            {"reasoning_tokens", 10}, {"records", 2}};
        Json source{{"state", "available"}, {"attempted_at_unix_ms", now}, {"last_success_unix_ms", now},
            {"from", "2026-09-05"}, {"to", "2026-10-04"}, {"source_records", 3}, {"unassigned_sessions", 1},
            {"skipped_sessions", 0}, {"skipped_reasons", Json::object()}, {"adapter_version", "shin-native-v1"}};
        Json input{{"schema_version", "shin.model-usage.v2"}, {"config_sha256", std::string(64, 'a')}, {"updated_at_unix_ms", now},
            {"projects", Json::array({{{"id", "project-zeta"}, {"name", "Project Zeta"}}, {{"id", "project-alpha"}, {"name", "Project Alpha"}}})},
            {"sources", {{"codex", source}, {"hermes", source}, {"opencode", source}}}, {"buckets", Json::array({row})}};
        auto result = summarize_model_usage(input, now);
        if (!result.at("fresh").get<bool>() || result.at("unassigned_sessions") != 3) return 1;
        const auto& all = result.at("projects").at(0);
        if (all.at("totals").at("input_tokens") != 100 || all.at("totals").at("output_tokens") != 20 ||
            all.at("totals").at("cache_read_tokens") != 200 || all.at("models").size() != 1) return 2;
        if (!result.at("projects").at(2).at("models").empty()) return 3;
        if (summarize_model_usage(input, now + 900001).at("fresh") != false) return 4;
        auto partial = input; partial["sources"]["codex"]["state"] = "partial";
        const auto partial_result = summarize_model_usage(partial, now);
        if (!partial_result.at("fresh").get<bool>() || partial_result.at("complete").get<bool>()) return 14;
        partial["sources"]["codex"]["state"] = "unavailable";
        if (summarize_model_usage(partial, now).at("fresh").get<bool>()) return 15;
        auto rejected = [&](Json bad) { try { (void)summarize_model_usage(bad, now); return false; } catch (...) { return true; } };
        auto bad = input; bad["buckets"].push_back(row); if (!rejected(bad)) return 5;
        auto bound = input;
        bound["schema_version"] = "shin.model-usage.v3";
        bound["projects"][0]["workspace_keys"] = Json::array({std::string(64,'a')});
        bound["projects"][1]["workspace_keys"] = Json::array({std::string(64,'b')});
        if (summarize_model_usage(bound, now).at("projects").at(2).at("workspace_keys") != Json::array({std::string(64,'a')})) return 16;
        bound["projects"][1]["workspace_keys"] = Json::array({std::string(64,'a')});
        if (!rejected(bound)) return 17;
        bound["projects"][0]["workspace_keys"] = Json::array({"not-a-workspace-digest"});
        if (!rejected(bound)) return 18;
        bad = input; bad["buckets"][0]["prompt"] = "DO-NOT-READ"; if (!rejected(bad)) return 6;
        bad = input; bad["sources"]["codex"]["adapter_version"] = "0.9.25"; if (!rejected(bad)) return 7;
        bad = input; bad["buckets"][0]["input_tokens"] = -1; if (!rejected(bad)) return 8;
        bad = input; bad["buckets"][0]["date"] = "2026-02-30"; if (!rejected(bad)) return 9;
        bad = input; bad["buckets"][0]["project"] = "unbound"; if (!rejected(bad)) return 10;
        bad = input; bad["buckets"][0]["measurement"] = "guessed"; if (!rejected(bad)) return 11;
        bad = input; bad["updated_at_unix_ms"] = now + 1; if (!rejected(bad)) return 12;
        auto aggregate = input; aggregate["buckets"][0]["measurement"] = "session-total";
        const auto aggregate_result = summarize_model_usage(aggregate, now);
        if (!aggregate_result.at("projects").at(0).at("days").empty() || aggregate_result.at("projects").at(0).at("totals").at("input_tokens") != 100) return 13;
        source_fixture(now);
        registered_fixture();
        std::cout << "native normalized model usage: projection, provenance, attribution and rejection passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 20; }
}
