#include "view.h"
#include "../ui/language.h"
#include "../ui/session_activity.h"
#include "../ui/project_diagnostics.h"
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
        Json warned{{"phase", "current"}, {"query_ready", true}, {"documentation_warnings", 1}};
        require(shin::ui::diagnostic_overview(warned, true).title == "Ready with documentation warnings");
        require(shin::ui::diagnostic_overview(warned, true).summary.find("1") != std::string::npos);
        warned["active_issue"] = true;
        require(shin::ui::diagnostic_overview(warned, true).title != "Ready with documentation warnings");
        tui::State navigation;
        tui::select_section(navigation, 2); tui::move_subpage(navigation, 1);
        require(navigation.tab == 4 && tui::main_section(navigation) == 2);
        tui::select_section(navigation, 3); require(navigation.tab == 7);
        tui::move_subpage(navigation, 1); require(navigation.tab == 3);
        tui::select_section(navigation, 2); require(navigation.tab == 4);
        tui::select_section(navigation, 3); require(navigation.tab == 3);
        tui::move_subpage(navigation, 1); require(navigation.tab == 6);
        navigation.update_confirmation = "reviewed-revision";
        tui::select_section(navigation, 0);
        require(navigation.update_confirmation.empty());
        tui::select_section(navigation, 3);
        require(navigation.tab == 6 && navigation.update_confirmation.empty());
        tui::move_subpage(navigation, 1); require(navigation.tab == 7);
        require(render(tui::Snapshot{}, navigation).find("Language") != std::string::npos);
        tui::Snapshot snapshot;
        snapshot.connected = true; snapshot.received = tui::Clock::now(); snapshot.error.clear();
        snapshot.service = {{"command", "service-status"}, {"service_running", true},
            {"telemetry", {{"project_states", Json::array({
                {{"root", "/example/a"}, {"id", "a"}, {"name", "Project A"}, {"phase", "current"}, {"query_ready", true},
                 {"source_coverage", {{"state", "partial"}, {"quarantined_files", 1}}}},
                {{"root", "/example/b"}, {"id", "b"}, {"name", "Project B"}, {"phase", "current"}, {"query_phase", "standby"},
                 {"quality_warning", {{"state", "degraded"}}}},
                {{"root", "/example/c"}, {"id", "c"}, {"name", "Project C"}, {"phase", "checking"}, {"query_ready", true}, {"active_issue", true}}
            })}, {"request_metrics", {{"recent", Json::array({
                {{"method", "query"}, {"purpose", "diagnostic"}, {"reader_state", "cold"}, {"duration_ms", 20}, {"service_duration_ms", 200}},
                {{"method", "query"}, {"purpose", "unclassified"}, {"reader_state", "warm"}, {"duration_ms", 10}, {"service_duration_ms", 100}}
            })}}}}}};
        const auto original = snapshot.service;
        tui::State state; state.tab = 1;
        auto value = render(snapshot, state);
        require(value.find("[partial]") != std::string::npos && value.find("standby") != std::string::npos && value.find("blocked") != std::string::npos);
        require(value.find("[search warning]") != std::string::npos);
        tui::select_project(snapshot, state, 1); require(state.project == "/example/b");
        std::swap(snapshot.service["telemetry"]["project_states"][0], snapshot.service["telemetry"]["project_states"][1]);
        state.detail = true;
        value = render(snapshot, state); require(value.find("/example/b") != std::string::npos && value.find("Project B") != std::string::npos);
        require(value.find("Ready with search warnings") != std::string::npos);
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
        snapshot.service["telemetry"]["mcp_activity"] = {{"schema_version", "shin.mcp-activity.v1"}, {"status", "ready"},
            {"attempts", 4}, {"calls", {{"query", 1}, {"status", 1}, {"help", 1}, {"projects", 1}}}, {"outcomes", {{"success", 4}}}};
        require(render(snapshot, state).find("MCP calls · history: 4") != std::string::npos);
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
        auto sessions = snapshot; state.tab = 5;
        require(render(tui::Snapshot{},state).find("Session activity is unavailable.") != std::string::npos);
        const std::string session_id(64,'a');
        sessions.service["telemetry"]["mcp_activity"] = {{"status","ready"},{"window_start_ms",0},{"dropped_updates",0},{"unconfirmed_updates",0},{"session_rows",Json::array({
            {{"session_id",session_id},{"context_calls",9},{"errors",1},{"tasks",2},{"recovery_requests",2},
             {"model_unknown",9},{"models",Json::object()},{"query_times",Json::array({3500})},{"queries_complete",true},
             {"timeline",Json::array({{{"method","query"},{"intent","context-recovery"},{"outcome","success"}}})}}
        })}};
        sessions.usage["context_events"] = {{"complete",true},{"events",Json::array({
            {{"session_id",session_id},{"at_ms",2000},{"kind","compaction"}},
            {{"session_id",session_id},{"at_ms",1000},{"kind","turn-model"},{"model_id","history-model"}}
        })}};
        sessions.service["telemetry"]["mcp_activity"]["session_rows"][0]["analysis"] = {
            {"schema_version","shin.session-analysis.v1"},{"calls_complete",true},{"unassigned_context_calls",1},
            {"reads",{{"reads",8},{"same_version_repeats",3},{"additional_versions",2},{"unknown_reads",1}}},
            {"episodes",Json::array({{{"start_ms",2000},{"kind","compaction"},{"closed",false},{"context_calls",4},
                {"errors",0},{"reads",{{"same_version_repeats",1},{"previously_seen_versions",2}}},{"interval_complete",false}}})}};
        auto observed = render(sessions,state);
        const auto presentation = shin::ui::session_activity(sessions.service.at("telemetry"),sessions.usage);
        require(presentation.available && !presentation.incomplete && presentation.sessions.size() == 1);
        require(presentation.sessions[0].id == session_id && presentation.sessions[0].calls == "9" &&
            presentation.sessions[0].errors == "1" && presentation.sessions[0].tasks == "2");
        require(presentation.notes.size() >= 4 && !presentation.sessions[0].details.empty());
        require(presentation.sessions[0].events.size() == 2 &&
            presentation.sessions[0].events.front().tone == shin::ui::SessionEventTone::host &&
            presentation.sessions[0].events.back().tone == shin::ui::SessionEventTone::success);
        auto event_states = sessions.service.at("telemetry");
        event_states["mcp_activity"]["session_rows"][0]["timeline"] = Json::array({
            {{"method","query"},{"outcome","tool-error"},{"at_ms",5000}},
            {{"method","read"},{"outcome","unfinished"},{"at_ms",6000}},
            {{"method","inspect"},{"outcome","future-outcome"},{"at_ms",7000}}});
        const auto journal = shin::ui::session_activity(event_states,sessions.usage).sessions[0].events;
        require(journal[0].tone == shin::ui::SessionEventTone::neutral && journal[0].at_ms == 7000);
        require(journal[1].tone == shin::ui::SessionEventTone::pending && journal[1].method == "read");
        require(journal[2].tone == shin::ui::SessionEventTone::error);
        // Fill the service projection, including more host markers than the
        // desktop can show. Display bounds must not change the source totals.
        auto full = sessions.service.at("telemetry");
        const auto base_row = full["mcp_activity"]["session_rows"][0];
        full["mcp_activity"]["session_rows"] = Json::array();
        Json host_events = Json::array();
        for (int i = 0; i < 8; ++i) {
            auto row = base_row;
            const std::string id(64,static_cast<char>('0'+i));
            row["session_id"] = id; row["context_calls"] = 90000;
            row["timeline"] = Json::array();
            for (int event = 0; event < 8; ++event) {
                row["timeline"].push_back({{"method","query"},{"outcome","success"},{"at_ms",10000+event}});
                host_events.push_back({{"session_id",id},{"kind","compaction"},{"at_ms",20000+event}});
            }
            full["mcp_activity"]["session_rows"].push_back(std::move(row));
        }
        const auto full_before = full;
        const auto bounded = shin::ui::session_activity(full,Json{{"context_events",{{"complete",true},{"events",host_events}}}});
        require(bounded.available && bounded.sessions.size() == 8 && full == full_before);
        std::size_t displayed = 0;
        for (const auto& session : bounded.sessions) {
            require(session.events.size() == 6 && session.calls == "90000");
            require(session.events.front().at_ms == 20007 && session.events.back().at_ms == 20002);
            displayed += session.events.size();
        }
        require(displayed == 48 && displayed == shin::ui::session_activity_max_journal_events);
        full["mcp_activity"]["session_rows"].push_back(base_row);
        require(!shin::ui::session_activity(full).available);
        auto empty_sessions = sessions.service.at("telemetry");
        empty_sessions["mcp_activity"]["session_rows"] = Json::array();
        require(shin::ui::session_activity(empty_sessions).available && shin::ui::session_activity(empty_sessions).sessions.empty());
        require(!shin::ui::session_activity(Json::object()).available);
        empty_sessions["projection"] = {{"truncated",true},{"omitted_rows",{{"/mcp_activity/session_rows",8}}}};
        const auto omitted_sessions = shin::ui::session_activity(empty_sessions);
        require(!omitted_sessions.available && omitted_sessions.projection_limited);
        require(shin::ui::session_activity_lines(empty_sessions).front() == "Session details were omitted to keep the service response bounded.");
        empty_sessions.erase("projection");
        empty_sessions["mcp_activity"]["session_rows"] = Json::array({{{"session_id","invalid"}}});
        require(shin::ui::session_activity(empty_sessions).sessions.empty());
        require(observed.find("Same version again: 3") != std::string::npos && observed.find("After host compaction") != std::string::npos);
        require(observed.find("Session aaaaaaaa") != std::string::npos && observed.find("1.5 s") != std::string::npos);
        require(observed.find("Models reported in calls: unknown") != std::string::npos && observed.find("Model in client history: history-model") != std::string::npos);
        sessions.service["telemetry"]["mcp_activity"]["session_rows"][0]["queries_complete"] = false;
        sessions.service["telemetry"]["mcp_activity"]["session_rows"][0]["query_times"] = Json::array({5000,4000});
        require(render(sessions,state).find("Next recorded query after compaction") == std::string::npos);
        shin::ui::set_language(shin::ui::Language::ru);
        require(render(sessions,state).find("Сессия aaaaaaaa") != std::string::npos);
        shin::ui::set_language(shin::ui::Language::en);
        snapshot.service = original; state.tab = 0; (void)render(snapshot, state); require(snapshot.service == original);
        std::cout << "TUI: states, partial/stale data, stable selection, timing filters, missing metrics and safe Unicode passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
