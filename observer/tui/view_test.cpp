#include "view.h"
#include <clocale>
#include <iostream>
#include <stdexcept>

using namespace shin::observer;
namespace {
void require(bool value) { if (!value) throw std::runtime_error("TUI presentation contract failed"); }
std::string render(const tui::Snapshot& snapshot, tui::State& state) {
    std::string out;
    for (const auto& line : tui::lines(snapshot, state)) out += line.text + '\n';
    return out;
}
}
int main() {
    try {
        std::setlocale(LC_CTYPE, "");
        tui::Snapshot snapshot;
        snapshot.connected = true; snapshot.received = tui::Clock::now(); snapshot.error.clear();
        snapshot.service = {{"command", "service-status"}, {"service_running", true},
            {"telemetry", {{"project_states", Json::array({
                {{"root", "/example/a"}, {"id", "a"}, {"name", "Project A"}, {"phase", "current"}, {"query_ready", true},
                 {"source_coverage", {{"state", "partial"}, {"quarantined_files", 1}}}},
                {{"root", "/example/b"}, {"id", "b"}, {"name", "Project B"}, {"phase", "current"}, {"query_phase", "standby"}},
                {{"root", "/example/c"}, {"id", "c"}, {"name", "Project C"}, {"phase", "checking"}, {"query_ready", true}, {"active_issue", true}}
            })}, {"request_metrics", {{"recent", Json::array({
                {{"method", "query"}, {"purpose", "diagnostic"}, {"reader_state", "cold"}, {"duration_ms", 20}, {"service_duration_ms", 200}},
                {{"method", "query"}, {"purpose", "unclassified"}, {"reader_state", "warm"}, {"duration_ms", 10}, {"service_duration_ms", 100}}
            })}}}}}};
        const auto original = snapshot.service;
        tui::State state; state.tab = 1;
        auto value = render(snapshot, state);
        require(value.find("[partial]") != std::string::npos && value.find("standby") != std::string::npos && value.find("blocked") != std::string::npos);
        tui::select_project(snapshot, state, 1); require(state.project == "/example/b");
        std::swap(snapshot.service["telemetry"]["project_states"][0], snapshot.service["telemetry"]["project_states"][1]);
        state.detail = true;
        value = render(snapshot, state); require(value.find("/example/b") != std::string::npos && value.find("Project B") != std::string::npos);
        snapshot.connected = false; snapshot.error = "service-unavailable";
        value = render(snapshot, state);
        require(value.find("offline") != std::string::npos && value.find("last successful observation") != std::string::npos);
        snapshot.connected = true; snapshot.received = tui::Clock::now() - std::chrono::seconds(7);
        require(render(snapshot, state).find("expired") != std::string::npos);
        snapshot.received = tui::Clock::now(); state.tab = 2; state.retained = false; state.traffic = 1;
        value = render(snapshot, state);
        require(value.find("Queries: 1") != std::string::npos && value.find("Search mean: 10 ms") != std::string::npos);
        state.traffic = 2;
        require(render(snapshot, state).find("Search mean: 20 ms") != std::string::npos);
        state.retained = true;
        require(render(snapshot, state).find("unavailable") != std::string::npos);
        state.tab = 3; snapshot.storage_error = "storage-timeout";
        value = render(snapshot, state); require(value.find("storage-timeout") != std::string::npos && value.find("0 MiB") == std::string::npos);
        state.tab = 4; snapshot.usage = {{"status", "unconfigured"}};
        require(render(snapshot, state).find("--usage-file") != std::string::npos);
        snapshot.usage = {{"status", "available"}, {"fresh", false}, {"complete", false}, {"projects", Json::array()}};
        require(render(snapshot, state).find("stale or incomplete") != std::string::npos);
        const auto unsafe = tui::terminal_text("unsafe\033]52;c;PAYLOAD\a\n\r\t\xC2\x9B\xE2\x80\xAE", 100);
        require(unsafe.find('\033') == std::string::npos && unsafe.find('\a') == std::string::npos && unsafe.find('\n') == std::string::npos);
        require(unsafe.find("\xC2\x9B") == std::string::npos && unsafe.find("\xE2\x80\xAE") == std::string::npos);
        require(tui::terminal_text("abcdef", 3) == "abc" && tui::terminal_text("x", 0).empty());
        require(tui::terminal_text("Проект", 6) == "Проект");
        require(tui::terminal_text("界a", 2) == "界");
        require(tui::terminal_text(std::string(1, '\xff'), 1) == "?");
        snapshot.service = original; state.tab = 0; (void)render(snapshot, state); require(snapshot.service == original);
        std::cout << "TUI: states, partial/stale data, stable selection, timing filters, missing metrics and safe Unicode passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
