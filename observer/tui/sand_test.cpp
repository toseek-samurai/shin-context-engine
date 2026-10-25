#include "sand.h"
#include "chart.h"
#include "../ui/language.h"
#include <ftxui/screen/screen.hpp>
#include <clocale>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <tuple>

using namespace shin::observer;
namespace {
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
tui::Snapshot fixture() {
    tui::Snapshot s;
    s.connected = true; s.received = tui::Clock::now(); s.error.clear();
    s.service = {{"service_running", true}, {"query_workers", 2}, {"telemetry", {
        {"schema_version", "shin.local-service-telemetry.v1"}, {"uptime_ms", 60000},
        {"project_states", Json::array({
            {{"name", "shin-context-engine"}, {"root", "/workspace/shin-context-engine"}, {"id", "shin"},
             {"phase", "current"}, {"query_ready", true}, {"source_coverage", {{"state", "complete"}}}},
            {{"name", "atlas-workspace"}, {"root", "/workspace/atlas-workspace"}, {"id", "atlas"},
             {"phase", "current"}, {"query_ready", true}, {"source_coverage", {{"state", "partial"}, {"quarantined_files", 1},
                {"files", Json::array({{{"path", "tests/example-config.txt"}, {"reason", "credential-assignment"}}})}}}},
            {{"name", "kestrel-api"}, {"root", "/workspace/kestrel-api"}, {"id", "api"},
             {"phase", "checking"}, {"query_ready", false}},
            {{"name", "console-web"}, {"root", "/workspace/console-web"}, {"id", "web"},
             {"phase", "current"}, {"query_ready", true}, {"active_issue", true}, {"error_code", "root-unavailable"}},
            {{"name", "docs-portal"}, {"root", "/workspace/docs-portal"}, {"id", "docs"},
             {"phase", "current"}, {"query_phase", "standby"}, {"source_coverage", {{"state", "complete"}}}}
        })},
        {"samples", Json::array()}, {"request_metrics", {{"query_p50_ms", 18.4}, {"responses_ready", 1382},
            {"response_errors", 2}, {"unobserved", 3}, {"recent", Json::array()}}},
        {"history", {{"status", "ready"}, {"rows", 1824}, {"captured_at_ms", 7200000}, {"oldest_at_ms", 0},
            {"minute_timeline", Json::array()}, {"timeline", Json::array()}}}
    }}};
    for (int i = 0; i <= 30; ++i) {
        const double memory = 72 + i * 0.8 + (i > 8 && i < 21 ? 36 : 0);
        s.service["telemetry"]["samples"].push_back({{"elapsed_ms", i * 2000},
            {"rss_bytes", i == 21 ? Json() : Json(memory * 1048576)}, {"workers", i > 15 ? 2 : 0}});
        s.service["telemetry"]["request_metrics"]["recent"].push_back({{"elapsed_ms", i * 2000},
            {"method", "query"}, {"purpose", i % 2 ? "unclassified" : "diagnostic"},
            {"duration_ms", i == 16 ? 58.0 : 18.0 + (i % 5)}});
    }
    auto& telemetry = s.service["telemetry"];
    auto& history = telemetry["history"];
    history["projects"] = Json::array();
    s.storage = {{"used_bytes", 684ULL * 1048576}, {"quota_bytes", 4096ULL * 1048576},
        {"shared_bytes", 46ULL * 1048576}, {"reclaimable_bytes", 0}, {"auto_clean", true}, {"projects", Json::array()}};
    const std::array requests{624, 416, 302, 198, 284}, sizes{114, 172, 138, 96, 118};
    for (std::size_t i = 0; i < 5; ++i) {
        auto& project = telemetry["project_states"][i];
        project["publications"] = 17 + i;
        project["last_success_age_ms"] = 4000;
        project["failures"] = i == 3 ? 2 : 0;
        if (project.contains("source_coverage") && !project["source_coverage"].contains("quarantined_files"))
            project["source_coverage"]["quarantined_files"] = 0;
        history["projects"].push_back({{"id", project["id"]}, {"requests", requests[i]}, {"errors", i == 3 ? 2 : 0}});
        s.storage["projects"].push_back({{"root", project["root"]}, {"used_bytes", sizes[i] * 1048576ULL},
            {"cache_bytes", (sizes[i] - 12) * 1048576ULL}, {"reason", "external-caches-only"}});
    }
    history["request_cohorts"] = Json::array();
    const auto cohort = [](const char* purpose, const char* reader, int count, int errors, double worker, double service) {
        return Json::array({0, "query", purpose, reader, count, errors, count, service, service + 5, worker, worker + 4,
            1.1, 2.4, 4.3, 1.2, 0.6, count, count, count, count, count, count});
    };
    history["request_cohorts"].push_back(cohort("unclassified", "warm", 180, 0, 18.4, 24.7));
    history["request_cohorts"].push_back(cohort("unclassified", "cold", 54, 2, 54.7, 89.5));
    history["request_cohorts"].push_back(cohort("diagnostic", "warm", 12, 0, 10.4, 20.0));
    s.usage = {{"status", "available"}, {"fresh", true}, {"complete", true}, {"unassigned_sessions", 2}, {"skipped_sessions", 0},
        {"projects", Json::array({{{"id", ""}, {"name", "All usage projects"},
            {"totals", {{"input_tokens", 1843120}, {"output_tokens", 241076}, {"cache_read_tokens", 3210480},
                {"cache_write_tokens", 42810}, {"reasoning_tokens", 80321}}},
            {"models", Json::array({
                {{"client", "example-client"}, {"model", "example-model-a"}, {"input_tokens", 1241034}, {"output_tokens", 154380}},
                {{"client", "example-client"}, {"model", "example-model-b"}, {"input_tokens", 602086}, {"output_tokens", 86696}}
            })}}})}};
    const std::string session_id(64,'a');
    telemetry["mcp_activity"] = {{"status","ready"},{"session_rows",Json::array({{{"session_id",session_id},
        {"context_calls",21},{"errors",2},{"tasks",3},{"recovery_requests",4},{"model_unknown",21},{"models",Json::object()},
        {"analysis",{{"schema_version","shin.session-analysis.v1"},{"calls_complete",true},{"unassigned_context_calls",3},
            {"reads",{{"reads",12},{"same_version_repeats",4},{"additional_versions",2},{"unknown_reads",1}}},
            {"episodes",Json::array({{{"start_ms",1792920000000LL},{"kind","compaction"},{"closed",true},
                {"context_calls",7},{"errors",1},{"interval_complete",true},
                {"reads",{{"same_version_repeats",2},{"previously_seen_versions",1}}}}})}}}}})}};
    return s;
}
struct Rendered {
    ftxui::Screen screen;
    tui::HitTargets hits;
    std::string plain;
};
Rendered render(const tui::Snapshot& s, tui::State& state, int width = 120, int height = 36, bool ascii = false, bool mono = false) {
    Rendered r{ftxui::Screen(width, height), {}, {}};
    ftxui::Render(r.screen, tui::sand_view(s, state, width, height, r.hits, ascii, mono));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) r.plain += r.screen.at(x, y).empty() ? " " : r.screen.at(x, y);
        r.plain += '\n';
    }
    return r;
}
void capture(const Rendered& r, const fs::path& directory, const std::string& name) {
    fs::create_directories(directory);
    std::ofstream(directory / (name + ".txt")) << r.plain;
    std::ofstream(directory / (name + ".ansi")) << r.screen.ToString();
    Json rows = Json::array();
    for (int y = 0; y < r.screen.dimy(); ++y) {
        auto row = Json::array();
        for (int x = 0; x < r.screen.dimx(); ++x) {
            const auto& cell = r.screen.CellAt(x, y);
            row.push_back({cell.character, cell.foreground_color.Print(false), cell.background_color.Print(true), cell.bold, cell.inverted});
        }
        rows.push_back(std::move(row));
    }
    std::ofstream(directory / (name + ".json")) << rows.dump();
}
void chart_tests(tui::Snapshot s) {
    auto& telemetry = s.service["telemetry"];
    auto series = tui::chart(s, 0, 0);
    require(series.points.size() == 31, "live samples lost");
    require(series.points[1].connect && !series.points[21].value && !series.points[22].connect, "null RSS bridged");
    telemetry["samples"][22]["elapsed_ms"] = 47000;
    require(!tui::chart(s, 0, 0).points[22].connect, "time gap bridged");
    telemetry["samples"] = Json::array();
    require(tui::chart(s, 0, 0).points.empty(), "missing samples became zeroes");
    telemetry["samples"] = Json::array();
    for (int i = 0; i < 121; ++i) telemetry["samples"].push_back({{"elapsed_ms", i}, {"rss_bytes", 0}});
    require(tui::chart(s, 0, 0).partial && tui::chart(s, 0, 0).points.empty(), "unbounded sample array");
    auto& history = telemetry["history"];
    const auto bucket = [](double at, const char* run) { return Json::array({at, 4, 0, 0, 36, 2, 20, 10485760, 2, 1, run}); };
    history["minute_timeline"] = Json::array({bucket(6900000, "a"), bucket(6960000, "a"),
        bucket(7020000, "b"), bucket(7140000, "b")});
    series = tui::chart(s, 0, 1);
    require(series.points.size() == 4 && series.points[1].connect && !series.points[2].connect && !series.points[3].connect,
        "history restarts or gaps joined");
    require(series.points[0].value == 10 && tui::chart(s, 2, 1).points[0].value == 18, "bucket means changed");
    history["minute_timeline"][1][7] = nullptr;
    require(!tui::chart(s, 0, 1).points[1].value, "null retained RSS became zero");
    history["minute_timeline"][0][5] = 0;
    require(!tui::chart(s, 2, 1).points[0].value, "zero timing samples produced mean");
    history["minute_timeline"][0][5] = 0.5;
    require(!tui::chart(s, 2, 1).points[0].value, "fractional sample count accepted");
    history["status"] = "unavailable";
    require(tui::chart(s, 1, 1).points.empty(), "unavailable history displayed");
    const auto all = tui::chart(s, 2, 0), normal = tui::chart(s, 2, 0, 1), diagnostic = tui::chart(s, 2, 0, 2);
    require(all.points.size() == normal.points.size() + diagnostic.points.size(), "query filter ignored");
    telemetry["request_metrics"]["recent"][0]["duration_ms"] = -1;
    require(!tui::chart(s, 2, 0).points[0].value, "invalid duration displayed");
}
}
int main(int argc, char** argv) {
    try {
        std::setlocale(LC_CTYPE, "");
        ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
        auto snapshot = fixture();
        chart_tests(snapshot);
        const auto original = snapshot.service;
        tui::State state;
        auto result = render(snapshot, state);
        require(result.plain.find(SHIN_OBSERVER_VERSION) != std::string::npos, "kit version absent");
        require(result.plain.find("Memory") != std::string::npos && result.plain.find("MiB") != std::string::npos, "memory graph absent");
        require(result.hits.project_count == 5, "overview did not fit five projects");
        require(result.plain.find("! Partial") != std::string::npos, "partial context hidden");
        require(result.plain.find("1382 / 2 / 3") != std::string::npos && result.plain.find("Uptime: ") != std::string::npos,
            "existing response counters or uptime lost");
        const auto glyph_color = [&](std::size_t project, const char* glyph) {
            const int y = result.hits.projects[project].y_min;
            for (int x = 0; x < result.screen.dimx(); ++x)
                if (result.screen.at(x, y) == glyph) return result.screen.CellAt(x, y).foreground_color;
            throw std::runtime_error("status symbol absent");
        };
        const auto ready_color = glyph_color(0, "●"), blocked_color = glyph_color(3, "×");
        require(ready_color != ftxui::Color::Default && ready_color != blocked_color, "Sand status colors lost");
        require(result.screen.CellAt(0, 0).background_color != ftxui::Color::Default, "Sand background lost");
        if (argc == 2) capture(result, argv[1], "overview-en");
        require(render(snapshot, state, 80, 24).hits.project_count == 5, "compact dashboard hides projects unnecessarily");
        state.tab = 1; result = render(snapshot, state);
        require(result.plain.find("blocked") != std::string::npos && result.plain.find("standby") != std::string::npos, "project states hidden");
        if (argc == 2) capture(result, argv[1], "projects-en");
        require(tui::project_status(snapshot.service["telemetry"]["project_states"][3], true).tone == tui::Tone::error, "blocked state not red");
        state.project = "/workspace/atlas-workspace"; state.detail = true;
        result = render(snapshot, state);
        require(result.plain.find("tests/example-config.txt") != std::string::npos &&
            result.plain.find("credential-assignment") != std::string::npos, "excluded file detail absent");
        if (argc == 2) capture(result, argv[1], "project-detail-en");
        for (const auto& [tab, name, expected] : std::array{
                std::tuple{2, "timing-en", "Queries: 246"},
                std::tuple{3, "storage-en", "684.0 MiB"},
                std::tuple{4, "models-en", "example-model-a"}}) {
            state = {}; state.tab = tab;
            result = render(snapshot, state);
            require(result.plain.find(expected) != std::string::npos, "gallery view omitted available measurements");
            if (argc == 2) capture(result, argv[1], name);
        }
        state = {}; state.tab = 1; state.detail = true;
        auto long_path = snapshot;
        const auto root = "/workspace/" + std::string(96, 'a') + "/tail-marker";
        long_path.service["telemetry"]["project_states"][0]["root"] = root;
        state.project = root;
        require(render(long_path, state, 80, 36).plain.find("tail-marker") != std::string::npos, "long project path clipped");
        auto unknown_coverage = snapshot;
        unknown_coverage.service["telemetry"]["project_states"][2]["source_coverage"]["state"] = "profile-revision-pending";
        state.detail = false;
        require(render(unknown_coverage, state).plain.find("profile-revision-pending") != std::string::npos, "unrecognized coverage text lost");
        for (auto language : {shin::ui::Language::en, shin::ui::Language::ru}) {
            shin::ui::set_language(language);
            for (const auto& [width, height] : {std::pair{60, 12}, {80, 24}, {120, 36}}) {
                for (int tab = 0; tab < 6; ++tab) {
                    state = {}; state.tab = tab;
                    result = render(snapshot, state, width, height);
                    require(result.plain.find("SHIN") != std::string::npos, "header clipped");
                    if (result.plain.find("q") == std::string::npos) throw std::runtime_error(
                        "quit hint clipped: " + std::to_string(width) + "x" + std::to_string(height) +
                        " tab " + std::to_string(tab) + "\n" + result.plain);
                }
            }
        }
        state = {}; result = render(snapshot, state);
        if (argc == 2) capture(result, argv[1], "overview-ru");
        shin::ui::set_language(shin::ui::Language::en);
        if (argc == 2) capture(render(snapshot, state, 80, 24), argv[1], "compact-en");
        snapshot.connected = false;
        result = render(snapshot, state);
        require(result.plain.find("OFFLINE") != std::string::npos &&
            result.plain.find("last successful observation") != std::string::npos, "stale data unlabeled");
        require(result.plain.find("96.0 MiB") == std::string::npos, "stale memory shown as live");
        if (argc == 2) capture(result, argv[1], "offline-en");
        snapshot.service["telemetry"]["project_states"] = Json::array();
        require(render(snapshot, state).plain.find("project-add") != std::string::npos, "empty-state guidance absent");
        result = render(snapshot, state, 40, 8);
        require(result.plain.find("resize") != std::string::npos, "small terminal guidance absent");
        snapshot.service = original; snapshot.connected = true;
        state = {};
        result = render(snapshot, state, 80, 24, true, true);
        require(result.plain.find('*') != std::string::npos, "ASCII graph absent");
        if (argc == 2) capture(result, argv[1], "compact-ascii");
        for (int y = 0; y < result.screen.dimy(); ++y)
            for (int x = 0; x < result.screen.dimx(); ++x)
                require(result.screen.CellAt(x, y).foreground_color == ftxui::Color::Default, "monochrome emitted colors");
        snapshot.service["telemetry"]["project_states"][0]["name"] = "unsafe\033]52;c;PAYLOAD\a";
        result = render(snapshot, state);
        require(result.plain.find('\033') == std::string::npos && result.plain.find('\a') == std::string::npos, "terminal injection");
        snapshot.service = original;
        state = {}; (void)render(snapshot, state);
        require(snapshot.service == original, "presentation mutated telemetry");
        std::cout << "Sand TUI: bounded graphs, gaps, run identity, states, EN/RU, layouts and terminal safety passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
