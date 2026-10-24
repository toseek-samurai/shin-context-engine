#include "shin/observer/local_telemetry.h"
#include "shin/observer/observer.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sqlite3.h>
#include <thread>

using namespace shin::observer;

namespace {
std::int64_t fixture_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void create_v2_history(const fs::path& directory) {
    ensure_private_directory(directory, true);
    const auto path = directory / "telemetry.sqlite";
    sqlite3* db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) throw std::runtime_error("open v2 history fixture");
    const char* schema =
        "PRAGMA page_size=4096; PRAGMA auto_vacuum=INCREMENTAL;"
        "CREATE TABLE events(id INTEGER PRIMARY KEY,run TEXT NOT NULL,at_ms INTEGER NOT NULL,kind TEXT NOT NULL,"
        "project TEXT,phase TEXT,method TEXT,outcome TEXT,duration REAL,bytes INTEGER,rss INTEGER,workers INTEGER,"
        "service_duration REAL,reader_state TEXT,purpose TEXT,error_code TEXT,admission REAL,documents REAL,source REAL,"
        "source_collection REAL,final_verification REAL,request_id TEXT,session_id TEXT,parent_request_id TEXT,follow_up TEXT,"
        "stage TEXT,wait_ms REAL,admission_retries REAL,generation_id TEXT,refresh_phases TEXT);"
        "CREATE INDEX events_time ON events(at_ms);"
        "PRAGMA application_id=1397246280; PRAGMA user_version=2;";
    char* error = nullptr;
    const int rc = sqlite3_exec(db, schema, nullptr, nullptr, &error);
    sqlite3_free(error);
    sqlite3_stmt* insert = nullptr;
    const int prepared = sqlite3_prepare_v2(db, "INSERT INTO events(run,at_ms,kind,project,phase,outcome,duration,error_code,refresh_phases) VALUES('prior-run',?1,'refresh','prior-project','blocked','error',7,'old-refresh-error','{}')", -1, &insert, nullptr);
    const int inserted = prepared == SQLITE_OK && sqlite3_bind_int64(insert, 1, fixture_now_ms()) == SQLITE_OK
        ? sqlite3_step(insert) : SQLITE_ERROR;
    sqlite3_finalize(insert);
    sqlite3_close(db);
    if (rc != SQLITE_OK || inserted != SQLITE_DONE) throw std::runtime_error("create v2 history fixture");
#ifndef _WIN32
    std::error_code ec;
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    if (ec) throw std::runtime_error("secure v2 history fixture");
#endif
}

bool wait_history(LocalTelemetry& telemetry, std::chrono::seconds timeout = std::chrono::seconds(30)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        const auto state = telemetry.snapshot().at("history");
        if (state.value("status", "") == "ready" && state.value("pending", std::size_t{1}) == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
}

int telemetry_scenario() {
    try {
        const Json failed_case{{"id", "case-one"}, {"query", "DO-NOT-EXPORT"},
            {"failure_reasons", Json::array({"missing-required-path"})}};
        const Json failed_gate{{"id", "eval"}, {"status", "failed"},
            {"result", {{"failures", Json::array({failed_case})}}}};
        const Json validation{{"gates", Json::array({failed_gate})}};
        const auto detail = validation_diagnostic(validation);
        if (detail.at("gates").at(0).at("cases").at(0).at("id") != "case-one" ||
            detail.dump().find("DO-NOT-EXPORT") != std::string::npos) return 21;
        const auto ready_detail = readiness_diagnostic({{"steps", Json::array({
            {{"id", "validate"}, {"status", "failed"}, {"result", validation}},
            {{"id", "worktree-diff"}, {"status", "failed"}, {"result", {{"current", false}, {"path", "DO-NOT-EXPORT"}}}},
            {{"id", "adoption-check"}, {"status", "failed"}, {"error", {{"code", "route-revision-conflict"}, {"message", "DO-NOT-EXPORT"}}}}
        })}});
        if (ready_detail["gates"][0] != detail["gates"][0] ||
            ready_detail["gates"][1]["reasons"] != Json::array({"worktree-not-current"}) ||
            ready_detail["gates"][2]["error_code"] != "route-revision-conflict" ||
            ready_detail.dump().find("DO-NOT-EXPORT") != std::string::npos) return 32;
        const auto compact_ready = readiness_diagnostic({{"steps", Json::array({
            {{"id", "validate"}, {"status", "failed"}, {"result", {{"failed_gates", Json::array({
                {{"id", "pack-eval"}, {"diagnostic", {{"cases", Json::array({
                    {{"id", "missing-owner"}, {"passed", false}, {"failure_reasons", Json::array({"missing-required-path"})},
                     {"missing_paths", Json::array({"/private/absolute.md", "../../outside.md", "C:/secret.md",
                         "folder\\private.md", "line\nbreak.md", std::string(161, 'x'),
                         "docs/expected.md", "guide/also.md", "other/third.md"})}}
                })}}}}
            })}}}}
        })}});
        if (compact_ready["gates"][0]["cases"][0]["id"] != "missing-owner" ||
            validation_failure_reason(compact_ready) != "missing-required-path" ||
            compact_ready["gates"][0]["cases"][0]["expected_paths"] != Json::array({"docs/expected.md", "guide/also.md"}) ||
            compact_ready.value("truncated", false) != true || compact_ready.dump().size() > 1024 ||
            compact_ready.dump().find("/private/absolute.md") != std::string::npos ||
            compact_ready.dump().find("../../outside.md") != std::string::npos ||
            compact_ready.dump().find("C:/secret.md") != std::string::npos ||
            bounded_validation_diagnostic(compact_ready) != compact_ready) return 33;
        for (const auto& path : {"/absolute.md", "../outside.md", "folder\\path.md", "C:/private.md",
                                 "line\nbreak.md"})
            if (!validation_expected_path(Json(path)).empty()) return 38;
        if (!validation_expected_path(Json(std::string(161, 'x'))).empty() ||
            validation_expected_path(Json("docs and guides/Проверка.md")) != "docs and guides/Проверка.md") return 39;
        const Json missing_case{{"id", "missing-owner"}, {"passed", false},
            {"failure_reasons", Json::array({"missing-required-path"})},
            {"missing_paths", Json::array({"docs/expected.md"})}};
        const Json eval_gate{{"id", "eval"}, {"status", "failed"}, {"result", {
            {"failure_reasons", Json::array({"query-failures"})},
            {"cases", Json::array({missing_case})}}}};
        const auto specific_over_summary = validation_diagnostic({{"gates", Json::array({eval_gate})}});
        if (validation_failure_reason(specific_over_summary) != "missing-required-path" ||
            validation_failure_expected_paths(specific_over_summary) != std::vector<std::string>{"docs/expected.md"}) return 34;
        for (const char* budget : {"diagnostic_budget", "failure_budget"}) {
            const Json header{{"id", "missing-owner"}, {"passed", false},
                {"detail_level", "header"}, {"details_omitted", true},
                {"failure_reasons", Json::array({"missing-required-path"})},
                {"primary_path", "docs/expected.md"}};
            const auto header_detail = validation_diagnostic({{"gates", Json::array({
                {{"id", "pack-eval"}, {"status", "failed"}, {"result", {
                    {"cases", Json::array({header})}, {budget, {{"truncated", true}}}}}}
            })}});
            if (header_detail["gates"][0]["cases"][0]["id"] != "missing-owner" ||
                validation_failure_reason(header_detail) != "missing-required-path" ||
                validation_failure_expected_paths(header_detail) != std::vector<std::string>{"docs/expected.md"} ||
                !header_detail.value("truncated", false)) return 40;
        }
        const Json doctor_failure{{"id", "doctor"}, {"reasons", Json::array({"secret-like-source-content"})},
            {"cases", Json::array()}};
        const Json eval_failure{{"id", "eval"}, {"reasons", Json::array({"query-failures"})},
            {"cases", Json::array({{{"id", "missing-owner"}, {"reasons", Json::array({"missing-required-path"})},
                {"expected_paths", Json::array({"docs/expected.md"})}}})}};
        const Json mixed_failure{{"schema_version", "shin.validation-diagnostic.v1"},
            {"gates", Json::array({doctor_failure, eval_failure})}};
        if (validation_failure_reason(mixed_failure) != "secret-like-source-content" ||
            !validation_failure_expected_paths(mixed_failure).empty()) return 37;
        auto excessive = detail;
        excessive["gates"] = Json::array();
        for (int i = 0; i < 100; ++i) excessive["gates"].push_back(detail.at("gates").at(0));
        const auto bounded = bounded_validation_diagnostic(excessive);
        if (bounded.dump().size() > 1024 || bounded.at("truncated") != true) return 22;
        excessive["gates"] = Json::array({{{"id", "secret body\n"}}, nullptr, 42});
        if (!bounded_validation_diagnostic(excessive).at("gates").empty()) return 23;
        if (!bounded_validation_diagnostic(Json{{"schema_version", 42}}).is_null()) return 24;
        const Json execution_gate{{"id", "check"}, {"status", "failed"}, {"result", nullptr},
            {"error", {{"code", "file-changed-during-verification"}, {"message", "DO-NOT-EXPORT"}}}};
        const auto execution = validation_diagnostic({{"gates", Json::array({execution_gate})}});
        if (execution.at("gates").at(0).value("error_code", "") != "file-changed-during-verification" ||
            execution.dump().find("DO-NOT-EXPORT") != std::string::npos) return 26;
        if (bounded_validation_diagnostic(execution) != execution) return 27;
        const auto findings = validation_diagnostic({{"gates", Json::array({
            {{"id", "doctor"}, {"status", "failed"}, {"result", {{"issues", Json::array({
                {{"id", "broken-local-link"}, {"severity", "error"}, {"path", "DO-NOT-EXPORT"}},
                {{"id", "info-only"}, {"severity", "info"}}})}}}},
            {{"id", "eval"}, {"status", "failed"}, {"result", {{"suite_failure_reasons", Json::array({"index-not-current"})}}}}
        })}});
        if (findings["gates"][0]["reasons"] != Json::array({"broken-local-link"}) ||
            findings["gates"][1]["reasons"] != Json::array({"index-not-current"}) ||
            findings.dump().find("DO-NOT-EXPORT") != std::string::npos) return 31;
        auto invalid_code = execution;
        for (const Json& code : {Json("SECRET/path\n"), Json(std::string(97, 'x')), Json(42), Json(nullptr)}) {
            invalid_code["gates"][0]["error_code"] = code;
            if (bounded_validation_diagnostic(invalid_code).at("gates").at(0).contains("error_code")) return 28;
        }
        auto maximum = execution;
        maximum["gates"] = Json::array();
        for (int i = 0; i < 8; ++i) {
            auto gate = detail.at("gates").at(0);
            gate["error_code"] = std::string(96, 'x');
            gate["cases"][0]["id"] = std::string(96, 'i');
            gate["cases"][0]["reasons"] = Json::array({std::string(96, 'r'), std::string(96, 's'), std::string(96, 't')});
            maximum["gates"].push_back(gate);
        }
        const auto limited = bounded_validation_diagnostic(maximum);
        if (limited.dump().size() > 1024 || limited.at("truncated") != true || limited.at("gates").empty()) return 29;
        Json crowded{{"schema_version", "shin.validation-diagnostic.v1"}, {"gates", Json::array()}};
        for (int i = 0; i < 4; ++i) {
            crowded["gates"].push_back({{"id", "eval-" + std::to_string(i)}, {"cases", Json::array({
                {{"id", "case-" + std::to_string(i)}, {"reasons", Json::array({"missing-required-path"})},
                 {"expected_paths", Json::array({std::string(150, 'a'), std::string(150, 'b')})}}
            })}});
        }
        const auto paths_limited = bounded_validation_diagnostic(crowded);
        if (paths_limited.dump().size() > 1024 || paths_limited.value("truncated", false) != true ||
            paths_limited.at("gates").size() != 4) return 35;
        for (const auto& gate : paths_limited.at("gates"))
            if (gate.at("cases").at(0).at("reasons") != Json::array({"missing-required-path"})) return 36;
        const Error structured("refresh-validation-failed", "Candidate rejected", detail);
        if (error_json(structured).at("diagnostic") != detail) return 25;
        const Error execution_error("refresh-validation-failed", "Candidate rejected", execution);
        if (error_json(execution_error).at("diagnostic") != execution) return 30;
        {
            LocalTelemetry paused_telemetry;
            Json paused_state{{"phase", "paused"}, {"ticks", 3}, {"publications", 0}, {"failures", 0},
                {"suspended_refreshes", 2}, {"retry_reason", "refresh-suspended"},
                {"refresh_authority", "private-bootstrap"},
                {"last_result", {{"ok", false}, {"error", {{"code", "refresh-suspended"}}}}}};
            paused_telemetry.record("/project/paused", paused_state);
            auto paused_project = paused_telemetry.snapshot().at("project_states").at(0);
            if (paused_project.at("active_issue") != false || paused_project.at("failures") != 0 ||
                paused_project.at("suspended_refreshes") != 2 || paused_project.at("retry_reason") != "refresh-suspended") return 58;
            paused_state["retry_reason"] = "untrusted/path";
            paused_telemetry.record("/project/paused", paused_state);
            paused_project = paused_telemetry.snapshot().at("project_states").at(0);
            if (paused_project.at("retry_reason") != "") return 59;
        }
        LocalTelemetry telemetry;
        Json state{{"phase", "checking"}, {"ticks", 0}, {"publications", 0}, {"failures", 0},
            {"refresh_authority", "private-bootstrap"}, {"last_result", nullptr}};
        telemetry.record("/project/one", state);
        if (!telemetry.snapshot().at("project_states").at(0).at("last_check_age_ms").is_null()) return 1;
        state.update({{"phase", "current"}, {"ticks", 1}, {"last_result", {{"source", "DO-NOT-EXPORT"}, {"ok", true}}}});
        state["change_monitor_restarts"] = 2;
        state["change_monitor_recoveries"] = 1;
        state["last_change_monitor_error"] = "path-unknown";
        telemetry.record("/project/one", state);
        for (int i = 0; i < 20; ++i) {
            state["phase"] = "checking";
            telemetry.record("/project/one", state);
            state["phase"] = "current";
            state["ticks"] = i + 2;
            telemetry.record("/project/one", state);
        }
        auto result = telemetry.snapshot();
        if (result.at("events").size() != 1 || result.dump().find("DO-NOT-EXPORT") != std::string::npos) return 2;
        if (result.at("project_states").at(0).at("last_check_age_ms").get<int>() < 0) return 3;
        if (result.at("project_states").at(0).at("change_monitor_restarts") != 2 ||
            result.at("project_states").at(0).at("change_monitor_recoveries") != 1 ||
            result.at("project_states").at(0).at("last_change_monitor_error") != "path-unknown") return 46;
        state.update({{"phase", "blocked"}, {"ticks", 22}, {"failures", 3},
            {"last_result", {{"ok", false}, {"error", {{"code", "refresh-validation-failed"}}}}}});
        telemetry.record("/project/one", state);
        auto project = telemetry.snapshot().at("project_states").at(0);
        if (!project.at("active_issue").get<bool>() || project.at("last_error_code") != "refresh-validation-failed") return 11;
        state["phase"] = "checking";
        telemetry.record("/project/one", state);
        project = telemetry.snapshot().at("project_states").at(0);
        if (!project.at("active_issue").get<bool>() || project.at("activity_age_ms").is_null()) return 12;
        state.update({{"phase", "current"}, {"ticks", 23}, {"last_result", {{"ok", true}}}});
        telemetry.record("/project/one", state);
        state["phase"] = "checking";
        telemetry.record("/project/one", state);
        project = telemetry.snapshot().at("project_states").at(0);
        if (project.at("active_issue").get<bool>() || project.at("failures") != 3 ||
            project.at("last_completed_phase") != "current" || project.at("last_success_age_ms").is_null() ||
            project.at("last_error_code") != "refresh-validation-failed" || project.at("error_code") != "") return 13;
        state.update({{"phase", "current"}, {"recoveries", 1}, {"last_recovery", "quality-qualification"}});
        telemetry.record("/project/one", state);
        result = telemetry.snapshot();
        if (result.at("project_states").at(0).at("recoveries") != 1 ||
            result.at("events").back().at("last_recovery") != "quality-qualification" ||
            result.at("project_states").at(0).at("active_issue") != false) return 14;
        state.update({{"phase", "coalescing"}, {"last_result", {{"ok", true}, {"recovery_pending", true}}}});
        telemetry.record("/project/one", state);
        if (telemetry.snapshot().at("project_states").at(0).at("recovering") != true) return 15;
        state["phase"] = "checking";
        telemetry.record("/project/one", state);
        if (telemetry.snapshot().at("project_states").at(0).at("recovering") != true) return 16;
        state["phase"] = "current";
        telemetry.record("/project/one", state);
        if (telemetry.snapshot().at("project_states").at(0).at("recovering") != false) return 17;
        for (int i = 1; i <= 200; ++i) {
            state["publications"] = i;
            telemetry.record("/project/one", state);
            telemetry.sample(0);
        }
        result = telemetry.snapshot();
        if (result.at("events").size() != 32 || result.at("samples").empty() || result.at("samples").size() > 120) return 4;
        for (int i = 0; i < 40; ++i) telemetry.record("/project/" + std::to_string(i), state);
        if (telemetry.snapshot().at("project_states").size() != 16) return 5;
        if (telemetry.snapshot().at("durable") != false) return 6;
        Json report{{"schema_version", "shin.worker-metric.v1"}, {"ok", true}, {"duration_ms", 20.0},
            {"response_bytes", std::uint64_t{1024}}, {"error_code", ""}, {"query", "DO-NOT-EXPORT"}};
        report["service_duration_ms"] = 80.0;
        report["reader_state"] = "cold";
        report["purpose"] = "diagnostic";
        report["phase_ms"] = {{"source", 12.0}, {"documents", -1}, {"admission", "DO-NOT-EXPORT"},
                              {"final_verification", 70000}, {"query", "DO-NOT-EXPORT"}};
        telemetry.request_result("/project/one", "status", report);
        if (!telemetry.snapshot().at("request_metrics").at("recent").empty()) return 8;
        const auto status_activity = telemetry.snapshot().at("activity");
        if (status_activity.at("calls").at("status") != 1 || status_activity.at("diagnostic") != 1 ||
            status_activity.at("calls").at("query") != 0) return 60;
        {
            LocalTelemetry isolated;
            isolated.record("/activity-project", state);
            auto correlated = report; correlated["session_id"] = std::string(64,'a');
            isolated.request_result("/activity-project", "query", correlated);
            isolated.request_result("/activity-project", "status", correlated);
            correlated["purpose"] = "unclassified";
            correlated["session_id"] = "PRIVATE-NOT-A-HASH";
            isolated.request_result("/activity-project", "read", correlated);
            auto snapshot = isolated.snapshot();
            const auto& a = snapshot.at("activity");
            if (a.at("reported_sessions") != 1 || a.at("uncorrelated") != 1 || a.at("diagnostic") != 2 ||
                a.at("calls").at("status") != 1 || a.at("calls").at("query") != 1 || a.at("calls").at("read") != 1 ||
                snapshot.at("project_states")[0].at("activity") != a ||
                snapshot.dump().find("PRIVATE-NOT-A-HASH") != std::string::npos) return 61;
            for (int i=0;i<300;++i) {
                correlated["session_id"] = digest("session-"+std::to_string(i));
                isolated.request_result("/activity-project", "status", correlated);
            }
            snapshot = isolated.snapshot();
            if (snapshot.at("activity").at("reported_sessions") != 256 ||
                snapshot.at("activity").at("sessions_truncated") != true ||
                snapshot.at("project_states")[0].at("activity").at("reported_sessions") != 64 ||
                snapshot.at("request_metrics").at("recent").size() != 2) return 62;
        }
        telemetry.request_result("/project/one", "query", report);
        const auto first = telemetry.snapshot().at("request_metrics").at("recent").back();
        if (first.at("service_duration_ms") != 80.0 || first.at("reader_state") != "cold" ||
            first.at("purpose") != "diagnostic" || first.at("phase_ms").at("source") != 12.0 ||
            !first.at("phase_ms").at("documents").is_null() || !first.at("phase_ms").at("admission").is_null() ||
            !first.at("phase_ms").at("final_verification").is_null() || first.dump().find("DO-NOT-EXPORT") != std::string::npos) return 32;
        {
            LocalTelemetry attribution;
            auto entry = report;
            entry["session_id"] = std::string(64, 'a');
            entry["model_id"] = "fixture-a";
            attribution.request_result("/one", "query", entry);
            entry["model_id"] = "fixture-b";
            attribution.request_result("/one", "read", entry);
            entry.erase("model_id");
            attribution.request_result("/one", "status", entry);
            entry["session_id"] = "invalid";
            entry["model_id"] = "fixture-a";
            attribution.request_result("/one", "status", entry);
            auto value = attribution.snapshot().at("activity");
            if (value["reported_sessions"] != 1 || value["models"]["fixture-a"] != 1 ||
                value["models"]["fixture-b"] != 1 || value["model_unknown"] != 2) return 90;
            entry["session_id"] = std::string(64, 'a');
            for (int i = 0; i < 20; ++i) {
                entry["model_id"] = "model-" + std::to_string(i);
                attribution.request_result("/one", "query", entry);
            }
            value = attribution.snapshot().at("activity");
            if (value["models"].size() != 16 || value["model_overflow"] != 6) return 91;
        }
        report["duration_ms"] = 40.0;
        report["ok"] = false;
        telemetry.request_result("/project/one", "query", report);
        telemetry.request_result("/project/one", "read", {{"error_code", "client-disconnected"}});
        auto metrics = telemetry.snapshot().at("request_metrics");
        if (metrics.at("responses_ready") != 1 || metrics.at("response_errors") != 1 || metrics.at("unobserved") != 1 ||
            metrics.at("query_p50_ms") != 20.0 || metrics.at("query_p95_ms") != 20.0 ||
            metrics.at("query_sample_count") != 1 ||
            metrics.dump().find("DO-NOT-EXPORT") != std::string::npos) return 9;
        for (int i = 0; i < 100; ++i) telemetry.request_result("/project/one", "read", report);
        metrics = telemetry.snapshot().at("request_metrics");
        if (metrics.at("recent").size() != 64 || !metrics.at("query_p50_ms").is_null()) return 10;
        {
            LocalTelemetry links;
            auto correlated = report;
            correlated["ok"] = true;
            correlated["request_id"] = std::string(64, 'a');
            correlated["session_id"] = std::string(64, 'b');
            correlated["generation_id"] = std::string(64, 'c');
            links.request_result("/one", "query", correlated);
            correlated["parent_request_id"] = correlated["request_id"];
            correlated["request_id"] = std::string(64, 'd');
            links.request_result("/two", "read", correlated);
            if (links.snapshot()["request_metrics"]["recent"].back()["follow_up"] != "unverified") return 40;
            correlated["generation_id"] = std::string(64, 'e');
            links.request_result("/one", "read", correlated);
            if (links.snapshot()["request_metrics"]["recent"].back()["follow_up"] != "unverified") return 41;
            correlated["generation_id"] = std::string(64, 'c');
            links.request_result("/one", "read", correlated);
            if (links.snapshot()["request_metrics"]["observed_follow_ups"] != 1) return 42;
            correlated["stage"] = "admission";
            correlated["session_id"] = "DO-NOT-EXPORT";
            links.request_result({}, "unknown", correlated);
            const auto rejection = links.snapshot()["request_metrics"]["recent"].back();
            if (rejection["project_id"] != "" || rejection["session_id"] != "" ||
                rejection["outcome"] != "admission-rejected" || rejection.dump().find("DO-NOT-EXPORT") != std::string::npos) return 43;
        }
        LocalTelemetry refusals;
        state.update({{"phase", "blocked"}, {"last_result", {{"ok", false},
            {"observation", {{"reason_code", "incomplete-source-observation"}}}}}});
        refusals.record("/project/one", state);
        if (refusals.snapshot().at("project_states").at(0).at("error_code") != "incomplete-source-observation") return 18;
        state["last_result"] = {{"ok", false}, {"diagnostic_ids", Json::array({"ready:validate:failed"})}};
        refusals.record("/project/one", state);
        if (refusals.snapshot().at("project_states").at(0).at("error_code") != "project-check-failed") return 19;
        state["last_result"] = {{"ok", false}, {"observation", {{"reason_code", "SECRET body/path"}}}};
        refusals.record("/project/one", state);
        if (refusals.snapshot().dump().find("SECRET") != std::string::npos) return 20;
        {
            const auto directory = fs::canonical(fs::temp_directory_path()) /
                ("shin-refresh-diagnostics-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            ensure_private_directory(directory, true);
            LocalTelemetry diagnostics;
            diagnostics.open_history(directory);
            const Json repeated{{"outcome", "error"}, {"phase", "blocked"}, {"duration_ms", 173.0},
                {"error_code", "refresh-validation-failed"}, {"last_stage", "candidate-validation"},
                {"active_ms", -1.0}, {"sleep_ms", std::numeric_limits<double>::infinity()},
                {"phases", {{"admission", 2.0}, {"candidate-validation", 121.0}, {"untrusted-stage", 99.0},
                    {"candidate-source-cache", 13.0}, {"source-cache-transfer", 17.0}}}};
            diagnostics.refresh_result("/private/project", repeated);
            diagnostics.refresh_result("/private/project", repeated);
            diagnostics.refresh_result("/private/project", {{"outcome", "error"}, {"phase", "unknown"}, {"duration_ms", 60087.0},
                {"error_code", "refresh-timeout"}, {"last_stage", "candidate-validation"},
                {"phases", {{"candidate-validation", 60080.0}}}});
            diagnostics.refresh_result("/private/project", {{"outcome", "success"}, {"phase", "coalescing"},
                {"duration_ms", 60090.0}, {"active_ms", 90.0}, {"sleep_ms", 60000.0},
                {"work", {{"source_files", 12}, {"source_bytes", 4096}, {"source_chunks", 4},
                    {"source_cache_builds", 1}, {"source_read_scan_ms", 20.0}, {"source_chunking_ms", 5.0},
                    {"source_index_ms", 7.0}, {"source_persist_ms", 3.0},
                    {"source_transfer_ms", 111.123456789}, {"source_transfer_verify_ms", 22.123456789},
                    {"source_transfer_snapshot_ms", 33.123456789}, {"validation_generation_runs", 1},
                    {"validation_reuses", 3}, {"validation_verified_source_files", 99999},
                    {"validation_verified_source_bytes", 999999999},
                    {"qualification_reused_output_bytes", 123456789}, {"qualification_reused_output_files", 16},
                    {"qualification_snapshot_ms", 12.123456789}, {"qualification_generation_ms", 10.123456789},
                    {"qualification_profile_ms", 8.123456789}, {"qualification_doctor_ms", 6.123456789},
                    {"qualification_adoption_ms", 4.123456789}, {"validation_profile-check_ms", 3.123456789},
                    {"validation_check_ms", 2.123456789}, {"validation_doctor_ms", 1.123456789},
                    {"validation_vacuum_ms", 8.123456789}, {"validation_eval_ms", 7.123456789},
                    {"validation_pack-eval_ms", 6.123456789}, {"validation_adoption-check_ms", 5.123456789},
                    {"raw_path", "/private/secret"},
                    {"negative", -1.0}, {"infinite", std::numeric_limits<double>::infinity()}}},
                {"error_code", ""}, {"last_stage", "qualification"},
                {"phases", {{"qualification", 4.0}}}});
            LocalTelemetry over_bound;
            over_bound.refresh_result("/private/project", {{"outcome", "error"}, {"duration_ms", 604800001.0},
                {"error_code", "refresh-timeout"}, {"last_stage", "observation"}});
            const auto unknown_duration = over_bound.snapshot().at("refresh_diagnostics").back();
            if (!unknown_duration.at("duration_ms").is_null() || unknown_duration.at("error_code") != "refresh-timeout") return 46;
            const auto current = diagnostics.snapshot().at("refresh_diagnostics");
            if (current.size() != 4 || current[0]["error_code"] != current[1]["error_code"] ||
                current[2]["last_stage"] != "candidate-validation" ||
                current[2]["duration_ms"] != 60087.0 || current[2]["phases"]["candidate-validation"] != 60080.0 ||
                current[0]["phases"]["candidate-source-cache"] != 13.0 || current[0]["phases"]["source-cache-transfer"] != 17.0 ||
                current[0]["phase"] != "blocked" || current[2]["phase"] != "unknown" || current[3]["phase"] != "coalescing" ||
                !current[0]["active_ms"].is_null() || !current[0]["sleep_ms"].is_null() ||
                current[3]["active_ms"] != 90.0 || current[3]["sleep_ms"] != 60000.0 ||
                current[3]["work"].size() != 29 || current[3]["work"].dump().size() <= 512 || current[3]["work"].contains("raw_path") ||
                current.dump().find("/private/project") != std::string::npos ||
                current.dump().find("untrusted-stage") != std::string::npos ||
                current.dump().find("/private/secret") != std::string::npos) {
                return 44;
            }
            Json persisted;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            do {
                persisted = diagnostics.snapshot().at("history");
                if (persisted.value("status", "") == "ready" &&
                    persisted.value("refresh_diagnostics", Json::array()).size() == 4) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            } while (std::chrono::steady_clock::now() < deadline);
            if (persisted.value("status", "") != "ready" || persisted["refresh_diagnostics"].size() != 4 ||
                persisted["refresh_diagnostics"][0]["phase"] != "coalescing" ||
                persisted["refresh_diagnostics"][0]["active_ms"] != 90.0 ||
                persisted["refresh_diagnostics"][0]["sleep_ms"] != 60000.0 ||
                persisted["refresh_diagnostics"][0]["work"].size() != 29 ||
                persisted["refresh_diagnostics"][0]["work"].contains("raw_path") ||
                persisted["refresh_diagnostics"][1]["error_code"] != "refresh-timeout" ||
                persisted["refresh_diagnostics"][1]["last_stage"] != "candidate-validation" ||
                persisted["refresh_diagnostics"][2]["error_code"] != persisted["refresh_diagnostics"][3]["error_code"]) return 45;
            diagnostics.close_history();
            std::error_code ignored;
            fs::remove_all(directory, ignored);
        }
        {
            const auto directory = fs::canonical(fs::temp_directory_path()) /
                ("shin-refresh-rollup-retention-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            ensure_private_directory(directory, true);
            LocalTelemetry durable;
            durable.open_history(directory);
            for (int i = 0; i < 1002; ++i) {
                durable.refresh_result("/private/retained", {{"outcome", "success"}, {"duration_ms", 20.0},
                    {"active_ms", 18.0}, {"sleep_ms", 2.0}});
                if ((i + 1) % 180 == 0 && !wait_history(durable)) return 55;
            }
            durable.refresh_result("/private/retained", {{"outcome", "error"}, {"duration_ms", 30.0}});
            durable.refresh_result("/private/retained", {{"outcome", "paused"}, {"duration_ms", 60000.0},
                {"active_ms", 5.0}, {"sleep_ms", 59995.0}, {"error_code", "refresh-suspended"}});
            if (!wait_history(durable)) return 56;
            const auto durable_history = durable.snapshot().at("history");
            const auto project_id = digest("/private/retained");
            std::uint64_t attempts = 0, successes = 0, errors = 0, pauses = 0;
            double wall_ms = 0, active_ms = 0, sleep_ms = 0, active_max_ms = 0;
            for (const auto& row : durable_history.at("refresh_aggregates")) {
                if (row.at("project_id") != project_id) continue;
                attempts += row.at("attempts").get<std::uint64_t>();
                successes += row.at("successes").get<std::uint64_t>();
                errors += row.at("errors").get<std::uint64_t>();
                pauses += row.at("paused").get<std::uint64_t>();
                wall_ms += row.at("wall_ms").get<double>();
                active_ms += row.at("active_ms").get<double>();
                sleep_ms += row.at("sleep_ms").get<double>();
                active_max_ms = std::max(active_max_ms, row.at("active_max_ms").get<double>());
            }
            if (attempts != 1004 || successes != 1002 || errors != 1 || pauses != 1 ||
                wall_ms != 80070.0 || active_ms != 18041.0 || sleep_ms != 61999.0 || active_max_ms != 18.0 ||
                durable_history.at("refresh_diagnostics").size() != 32 ||
                durable_history.at("pruned_current_run").get<std::uint64_t>() < 2) return 57;
            durable.close_history();
            std::error_code ignored;
            fs::remove_all(directory, ignored);
        }
        {
            const auto directory = fs::canonical(fs::temp_directory_path()) /
                ("shin-refresh-rollup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            create_v2_history(directory);
            LocalTelemetry upgraded;
            upgraded.open_history(directory);
            upgraded.refresh_result("/private/paused", {{"outcome", "paused"}, {"phase", "paused"},
                {"duration_ms", 604800000.0}, {"active_ms", 1.0}, {"sleep_ms", 604799999.0},
                {"error_code", "refresh-suspended"}, {"last_stage", "source-cache-transfer"}});
            if (!wait_history(upgraded)) return 47;
            auto history = upgraded.snapshot().at("history");
            if (history.value("status", "") != "ready" || history.value("rows", 0) != 2 ||
                history.at("refresh_diagnostics").size() != 2 ||
                history.at("refresh_diagnostics")[0].at("outcome") != "paused" ||
                history.at("refresh_diagnostics")[0].at("sleep_ms") != 604799999.0 ||
                history.at("refresh_diagnostics")[1].at("error_code") != "old-refresh-error" ||
                history.at("refresh_aggregate_coverage").at("historical_backfill") != false ||
                history.at("refresh_aggregates").size() != 1 ||
                history.at("refresh_aggregates")[0].at("attempts") != 1 ||
                history.at("refresh_aggregates")[0].at("errors") != 0 ||
                history.at("refresh_aggregates")[0].at("paused") != 1) return 48;
            upgraded.close_history();

            // Seed more than the rollup cap plus an expired bucket, then let
            // the next transactional event enforce both retention bounds.
            sqlite3* db = nullptr;
            if (sqlite3_open((directory / "telemetry.sqlite").string().c_str(), &db) != SQLITE_OK) return 49;
            const auto now_hour = (fixture_now_ms() / 3600000) * 3600000;
            char* sql_error = nullptr;
            const int cleared = sqlite3_exec(db, "DELETE FROM refresh_hourly", nullptr, nullptr, &sql_error);
            sqlite3_free(sql_error);
            sqlite3_stmt* seed = nullptr;
            const int seed_prepare = sqlite3_prepare_v2(db, "INSERT INTO refresh_hourly(project,hour_ms,attempts,successes,errors,paused,wall_sum,wall_samples,active_sum,active_samples,sleep_sum,sleep_samples,active_max) VALUES(?1,?2,1,1,0,0,1,1,1,1,0,0,1)", -1, &seed, nullptr);
            bool seeded = cleared == SQLITE_OK && seed_prepare == SQLITE_OK;
            for (int i = 0; seeded && i < 4097; ++i) {
                sqlite3_reset(seed);
                const auto name = "seed-" + std::to_string(i);
                seeded = sqlite3_bind_text(seed, 1, name.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                    sqlite3_bind_int64(seed, 2, now_hour) == SQLITE_OK && sqlite3_step(seed) == SQLITE_DONE;
            }
            if (seeded) {
                sqlite3_reset(seed);
                seeded = sqlite3_bind_text(seed, 1, "expired", -1, SQLITE_STATIC) == SQLITE_OK &&
                    sqlite3_bind_int64(seed, 2, now_hour - 8LL * 24 * 3600 * 1000) == SQLITE_OK && sqlite3_step(seed) == SQLITE_DONE;
            }
            sqlite3_finalize(seed);
            sqlite3_close(db);
            if (!seeded) return 50;

            LocalTelemetry pruned;
            pruned.open_history(directory);
            pruned.refresh_result("/post-prune", {{"outcome", "success"}, {"duration_ms", 12.0}});
            if (!wait_history(pruned)) return 51;
            const auto bounded_aggregates = pruned.snapshot().at("history").at("refresh_aggregates");
            if (bounded_aggregates.size() > 16 * 24) return 52;
            pruned.close_history();
            if (sqlite3_open((directory / "telemetry.sqlite").string().c_str(), &db) != SQLITE_OK) return 53;
            sqlite3_stmt* counts = nullptr;
            const int count_prepare = sqlite3_prepare_v2(db, "SELECT count(*),sum(hour_ms<?1),max(hour_ms) FROM refresh_hourly", -1, &counts, nullptr);
            const bool count_ok = count_prepare == SQLITE_OK &&
                sqlite3_bind_int64(counts, 1, fixture_now_ms() - 7LL * 24 * 3600 * 1000) == SQLITE_OK &&
                sqlite3_step(counts) == SQLITE_ROW && sqlite3_column_int64(counts, 0) == 4096 &&
                sqlite3_column_int64(counts, 1) == 0;
            sqlite3_finalize(counts);
            sqlite3_close(db);
            std::error_code ignored;
            fs::remove_all(directory, ignored);
            if (!count_ok) return 54;
        }
        Json status{{"ok", true}, {"telemetry", telemetry.snapshot()},
            {"resident_errors", {{"/project/one", "refresh-validation-failed"}}}};
        const auto small = status;
        bound_local_service_status(status);
        if (status.at("telemetry").at("projection").at("truncated") != false ||
            status.at("telemetry").at("request_metrics") != small.at("telemetry").at("request_metrics")) return 33;
        auto& data = status["telemetry"];
        for (auto& row : data["project_states"]) {
            row["root"] = "/long/" + std::string(400, 'r') + row.at("id").get<std::string>();
            row["validation_diagnostic"] = limited;
        }
        data["samples"] = Json::array();
        for (int i = 0; i < 120; ++i)
            data["samples"].push_back({{"elapsed_ms", 123456789 + i}, {"rss_bytes", 123456789},
                                      {"workers", 4}, {"requests_started", 123456789}});
        auto& history = data["history"];
        history = {{"status", "ready"}, {"rows", 20000}, {"request_cohorts_truncated", false},
            {"timeline", Json::array()}, {"minute_timeline", Json::array()},
            {"request_cohorts", Json::array()}, {"events", Json::array()}};
        for (int i = 0; i < 169; ++i) history["timeline"].push_back(Json(10, 12345.678901234567));
        for (int i = 0; i < 61; ++i) {
            auto row = Json(10, 12345.678901234567);
            row.push_back(std::string(64, 'r'));
            history["minute_timeline"].push_back(row);
        }
        for (int i = 0; i < 48; ++i) {
            auto row = Json::array({std::string(64, 'p'), "query", "unclassified", "warm"});
            for (int j = 0; j < 18; ++j) row.push_back(12345.678901234567);
            history["request_cohorts"].push_back(row);
        }
        for (int i = 0; i < 16; ++i) history["events"].push_back({{"at_ms", 16 - i}, {"phase", "current"}});
        const auto full = status;
        if (full.dump().size() <= 65536) return 34;
        bound_local_service_status(status);
        if (status.dump().size() > kLocalStatusByteLimit || data["projection"]["truncated"] != true ||
            data["project_states"] != full["telemetry"]["project_states"] ||
            status["resident_errors"] != full["resident_errors"] || history["rows"] != 20000) return 35;
        for (const auto* key : {"responses_ready", "response_errors", "query_sample_count", "query_p95_ms"})
            if (data["request_metrics"][key] != full["telemetry"]["request_metrics"][key]) return 36;
        for (const auto& [path, count] : data["projection"]["omitted_rows"].items()) {
            const Json::json_pointer pointer(path);
            const auto& original = full["telemetry"].at(pointer);
            const auto& projected = data.at(pointer);
            const auto removed = count.get<std::size_t>();
            if (projected.size() + removed != original.size()) return 37;
            const bool newest_first = path == "/history/events" || path == "/history/request_cohorts";
            for (std::size_t i = 0; i < projected.size(); ++i)
                if (projected[i] != original[i + (newest_first ? 0 : removed)]) return 38;
        }
        const auto projected = status;
        bound_local_service_status(status);
        if (status != projected) return 39;
        status["essential"] = std::string(65536, 'x');
        try { bound_local_service_status(status); return 40; }
        catch (const Error& error) { if (error.code != "service-status-too-large") return 41; }
        std::cout << "bounded native telemetry passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 7; }
}

int main() {
    const int result = telemetry_scenario();
    if (result) std::cerr << "native telemetry check failed: " << result << '\n';
    return result;
}
