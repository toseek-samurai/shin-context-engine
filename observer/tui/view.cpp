#include "view.h"

#include "../ui/project_diagnostics.h"
#include "../ui/request_timing.h"
#include "../ui/service_activity.h"
#include "../ui/session_activity.h"
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <sstream>

namespace shin::observer::tui {
namespace {
const Json& field(const Json& value, const char* key) {
    static const Json absent;
    const auto at = value.is_object() ? value.find(key) : value.end();
    return at == value.end() ? absent : *at;
}
std::string text(const Json& value) { return value.is_string() ? value.get<std::string>() : shin::ui::tr("unknown"); }
std::string number(const Json& value, double divisor = 1) {
    if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < 0) return shin::ui::tr("unknown");
    std::ostringstream out;
    out << std::fixed << std::setprecision(divisor == 1 ? 0 : 1) << value.get<double>() / divisor;
    return ui::decimal(out.str());
}
std::string mean(const ui::TimingMean& value) { return value.value() ? number(*value.value(), 1.0) + " ms" : shin::ui::tr("unknown"); }
const Json& projects(const Snapshot& snapshot) { return field(field(snapshot.service, "telemetry"), "project_states"); }
std::string key(const Json& project) { return text(field(project, "root")); }
std::string state_name(const Json& row, bool connected) {
    using ui::ProjectState;
    switch (ui::project_state(row, connected)) {
    case ProjectState::offline: return shin::ui::tr("offline");
    case ProjectState::checking: return shin::ui::tr("checking");
    case ProjectState::pending: return shin::ui::tr("preparing");
    case ProjectState::recovering: return shin::ui::tr("recovering");
    case ProjectState::blocked: return shin::ui::tr("blocked");
    case ProjectState::current: return shin::ui::tr("ready");
    case ProjectState::standby: return shin::ui::tr("standby");
    case ProjectState::stopped: return shin::ui::tr("stopped");
    default: return shin::ui::tr("unknown");
    }
}
}

bool fresh(const Snapshot& s) { return s.connected && Clock::now() - s.received < std::chrono::seconds(6); }

Line project_status(const Json& row, bool connected) {
    using ui::ProjectState;
    const auto state = ui::project_state(row, connected);
    const auto tone = state == ProjectState::current ? Tone::good : state == ProjectState::blocked ? Tone::error :
        state == ProjectState::standby || state == ProjectState::stopped ? Tone::idle :
        state == ProjectState::checking || state == ProjectState::pending || state == ProjectState::recovering ? Tone::info : Tone::muted;
    const char* symbol = state == ProjectState::current ? "● " : state == ProjectState::blocked ? "× " :
        state == ProjectState::standby || state == ProjectState::stopped ? "○ " :
        tone == Tone::info ? "↻ " : "? ";
    return {std::string(symbol) + state_name(row, connected), tone};
}

std::string terminal_text(std::string_view input, int columns, bool pad) {
    // Decode before checking: C1 controls, bidi controls and malformed UTF-8
    // must not reach the terminal. Never split a wide character at the edge.
    std::string result;
    std::mbstate_t decoder{};
    int used = 0;
    for (std::size_t at = 0; at < input.size() && used < columns && at < 8192;) {
        wchar_t value = 0;
        // Explicit per-call conversion state; never uses mbrtowc's shared state.
        const auto length = std::mbrtowc(&value, input.data() + at, input.size() - at, &decoder); // NOLINT(concurrency-mt-unsafe)
        if (length == static_cast<std::size_t>(-1) || length == static_cast<std::size_t>(-2) || length == 0) {
            result += '?'; ++used; ++at; decoder = {}; continue;
        }
        const int width = ::wcwidth(value);
        if (!std::iswprint(value) || width < 0 || (value >= 0x202a && value <= 0x202e) ||
            (value >= 0x2066 && value <= 0x2069)) { result += '?'; ++used; }
        else {
            if (used + width > columns) break;
            result.append(input.substr(at, length)); used += width;
        }
        at += length;
    }
    if (pad && used < columns) result.append(static_cast<std::size_t>(columns - used), ' ');
    return result;
}

void select_project(const Snapshot& snapshot, State& state, int direction) {
    const auto& rows = projects(snapshot);
    if (!rows.is_array() || rows.empty()) { state.project.clear(); state.detail = false; return; }
    auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return key(row) == state.project; });
    const auto index = found == rows.end() ? 0 : static_cast<int>(found - rows.begin());
    const auto next = std::clamp(index + direction, 0, static_cast<int>(rows.size()) - 1);
    state.project = key(rows[static_cast<std::size_t>(next)]);
}

std::vector<Line> lines(const Snapshot& snapshot, State& state) {
    std::vector<Line> out;
    const bool connected = fresh(snapshot);
    if (state.help) return {
        {shin::ui::tr("Keyboard"), Tone::heading}, {shin::ui::tr("1-6 / Tab / Left / Right  Switch view")},
        {shin::ui::tr("Up / Down or j / k         Select a project, otherwise scroll")},
        {shin::ui::tr("Enter                     Project details")}, {shin::ui::tr("Esc / Backspace           Back")},
        {shin::ui::tr("PageUp / PageDown          Scroll a page")},
        {shin::ui::tr("t                         Recent / retained request timings")},
        {shin::ui::tr("f                         All / unclassified / diagnostic queries")},
        {shin::ui::tr("p                         Next model-usage project")},
        {shin::ui::tr("r                         Refresh (storage is refreshed on demand)")},
        {shin::ui::tr("g / h                     Chart metric / Live, 1 h, 24 h")},
        {shin::ui::tr("Mouse                     Select tabs, metrics, periods and projects")},
        {shin::ui::tr("? / Ctrl-L                Help / redraw screen")}, {shin::ui::tr("q / Ctrl-C                Quit")}, {shin::ui::tr("l                         Switch English / Russian")}, {},
        {shin::ui::tr("Read-only monitor. No service startup, enrollment, cleanup or usage collection.")},
        {shin::ui::tr("Service observations are not new source audits or answer-quality scores.")}};
    if (!connected) {
        out.push_back({snapshot.error == "connecting" ? shin::ui::tr("Connecting to local service...") : shin::ui::tr("Service offline / observation unavailable"), Tone::warning});
        out.push_back({snapshot.connected ? shin::ui::tr("Last service response expired.") : snapshot.error});
        out.push_back({shin::ui::tr("Reconnects automatically. Use shin-ce service-status --text for diagnostics.")});
        if (!snapshot.service.is_null()) out.push_back({shin::ui::tr("Retained values below are from the last successful observation."), Tone::warning});
        out.push_back({});
    }
    const auto& telemetry = field(snapshot.service, "telemetry");
    if (state.tab == 0) {
        out.push_back({shin::ui::tr("Service overview"), Tone::heading});
        if (!snapshot.service.is_null()) {
            out.push_back({shin::ui::tr("Service: ") + std::string(field(snapshot.service, "service_running") == true ? shin::ui::tr("running") : shin::ui::tr("not running"))});
            out.push_back({shin::ui::tr("Uptime: ") + number(field(telemetry, "uptime_ms"), 1000) + shin::ui::tr(" s")});
            out.push_back({shin::ui::tr("Active queries: ") + number(field(snapshot.service, "query_workers"))});
            const auto& samples = field(telemetry, "samples");
            const auto rss = samples.is_array() && !samples.empty() ? field(samples.back(), "rss_bytes") : Json();
            out.push_back({shin::ui::tr("RAM (service + workers): ") + number(rss, 1048576) + shin::ui::tr(" MiB")});
            const auto& metrics = field(telemetry, "request_metrics");
            out.push_back({shin::ui::tr("Ready responses: ") + number(field(metrics, "responses_ready"))});
            out.push_back({shin::ui::tr("Response errors: ") + number(field(metrics, "response_errors"))});
            out.push_back({shin::ui::tr("Unobserved responses: ") + number(field(metrics, "unobserved"))});
        }
        else out.push_back({shin::ui::tr("No service measurements yet.")});
        const auto& history = field(telemetry, "history");
        out.push_back({});
        out.push_back({std::string(shin::ui::tr("History: ")) + shin::ui::tr(text(field(history, "status")).c_str()) + shin::ui::tr(" | Retained records: ") + number(field(history, "rows"))});
    } else if (state.tab == 1) {
        out.push_back({shin::ui::tr("Projects"), Tone::heading});
        const auto& rows = projects(snapshot);
        select_project(snapshot, state, 0);
        if (!rows.is_array() || rows.empty()) out.push_back({shin::ui::tr("No observed projects. Connect a folder with shin-ce project-add --root PATH.")});
        else if (!state.detail) {
            out.push_back({shin::ui::tr("SEARCH       OBSERVER       PROJECT")});
            for (const auto& row : rows) {
                std::ostringstream line;
                line << std::left << terminal_text(state_name(row, connected), 15, true)
                    << terminal_text(shin::ui::tr(text(field(row, "phase")).c_str()), 15, true) << text(field(row, "name"));
                const auto partial = field(field(row, "source_coverage"), "state") == "partial";
                if (partial) line << shin::ui::tr(" [partial]");
                out.push_back({line.str(), key(row) == state.project ? Tone::selected :
                    ui::active_issue(row) || partial ? Tone::warning : Tone::normal});
            }
            out.push_back({}); out.push_back({shin::ui::tr("Up/Down select  |  Enter details")});
        } else {
            const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return key(row) == state.project; });
            if (found != rows.end()) {
                const auto& row = *found;
                out.push_back({text(field(row, "name")), Tone::heading}); out.push_back({state.project});
                const auto status = project_status(row, connected);
                out.push_back(status);
                out.push_back({shin::ui::tr("Search: ") + state_name(row, connected) + shin::ui::tr(" | Observer: ") + shin::ui::tr(text(field(row, "phase")).c_str())});
                const auto explanation = ui::diagnostic_overview(row, connected);
                out.push_back({explanation.title, status.tone});
                out.push_back({explanation.summary});
                out.push_back({explanation.next, Tone::muted});
                out.push_back({shin::ui::tr("Last successful check (at observation): ") + number(field(row, "last_success_age_ms"), 1000) + shin::ui::tr(" s ago")});
                out.push_back({shin::ui::tr("Published updates: ") + number(field(row, "publications"))});
                out.push_back({shin::ui::tr("Active issue: ") + std::string(ui::active_issue(row) ? shin::ui::tr("yes") : shin::ui::tr("no"))});
                out.push_back({shin::ui::tr("Issue code: ") + ui::error_code(row, "error_code")});
                const auto& coverage = field(row, "source_coverage");
                out.push_back({std::string(shin::ui::tr("Coverage: ")) + shin::ui::tr(text(field(coverage, "state")).c_str()) + shin::ui::tr(" | Quarantined files: ") + number(field(coverage, "quarantined_files"))});
                const auto& excluded = field(coverage, "files");
                if (excluded.is_array()) for (std::size_t i = 0; i < std::min<std::size_t>(excluded.size(), 8); ++i) {
                    const auto& file = excluded[i];
                    out.push_back({"! " + text(field(file, "path")), Tone::warning});
                    out.push_back({"  " + text(field(file, "reason")), Tone::muted});
                }
                const auto& histories = field(field(telemetry, "history"), "projects");
                if (histories.is_array()) for (const auto& item : histories) if (field(item, "id") == field(row, "id"))
                    out.push_back({shin::ui::tr("Retained requests: ") + number(field(item, "requests")) + shin::ui::tr(" | Errors: ") + number(field(item, "errors"))});
                out.push_back({}); out.push_back({shin::ui::tr("Esc back  |  shin-ce profile-review --root PATH for pending configuration changes")});
                for (const auto& line : ui::service_activity_lines(row)) out.push_back({line});
            }
        }
    } else if (state.tab == 2) {
        out.push_back({shin::ui::tr("Request timing"), Tone::heading});
        const std::array filters{shin::ui::tr("all queries"), shin::ui::tr("unclassified queries"), shin::ui::tr("diagnostic queries")};
        out.push_back({std::string(state.retained ? shin::ui::tr("Retained history") : shin::ui::tr("Recent requests")) + " | " + filters[static_cast<std::size_t>(state.traffic)]});
        const auto timing = ui::request_timing(telemetry, state.retained, state.traffic);
        if (!timing.available) out.push_back({shin::ui::tr("Timing measurements unavailable.")});
        else {
            if (timing.truncated || timing.invalid) out.push_back({shin::ui::tr("Partial measurements; omitted/invalid rows are not treated as zero."), Tone::warning});
            out.push_back({shin::ui::tr("Queries: ") + std::to_string(timing.requests) + shin::ui::tr(" | Errors: ") + std::to_string(timing.errors)});
            out.push_back({shin::ui::tr("Search mean: ") + mean(timing.worker) + shin::ui::tr(" | Including service: ") + mean(timing.service)});
            out.push_back({});
            const std::array names{shin::ui::tr("Warm reader"), shin::ui::tr("With preparation"), shin::ui::tr("Unknown reader")};
            for (std::size_t i = 0; i < names.size(); ++i)
                out.push_back({std::string(names[i]) + ": " + mean(timing.readers[i].service) + " (" + std::to_string(timing.readers[i].requests) + shin::ui::tr(" queries)")});
            out.push_back({});
            const std::array phases{shin::ui::tr("Admission"), shin::ui::tr("Documents"), shin::ui::tr("Source"), shin::ui::tr("Collection"), shin::ui::tr("Verification")};
            for (std::size_t i = 0; i < phases.size(); ++i) {
                std::ostringstream line; line << std::left << terminal_text(phases[i], 16, true) << terminal_text(mean(timing.phases[i]), 12, true);
                out.push_back({line.str()});
            }
        }
        out.push_back({}); out.push_back({shin::ui::tr("t recent/history  |  f traffic filter  |  Means are measured samples, not SLA guarantees.")});
        for (const auto& line : ui::service_activity_lines(telemetry)) out.push_back({line});
    } else if (state.tab == 5) {
        for (const auto& line : ui::session_activity_lines(telemetry,snapshot.usage)) out.push_back({line});
    } else if (state.tab == 3) {
        out.push_back({shin::ui::tr("Storage"), Tone::heading});
        if (snapshot.storage_loading) out.push_back({shin::ui::tr("Reading storage inventory..."), Tone::warning});
        if (!snapshot.storage_error.empty()) out.push_back({shin::ui::tr("Storage unavailable: ") + snapshot.storage_error, Tone::warning});
        const auto& value = snapshot.storage;
        if (!value.is_null()) {
            out.push_back({shin::ui::tr("Logical files: ") + number(field(value, "used_bytes"), 1048576) + shin::ui::tr(" MiB | Budget: ") + number(field(value, "quota_bytes"), 1048576) + shin::ui::tr(" MiB")});
            out.push_back({shin::ui::tr("Reclaimable detached caches: ") + number(field(value, "reclaimable_bytes"), 1048576) + shin::ui::tr(" MiB")});
            out.push_back({shin::ui::tr("Shared data: ") + number(field(value, "shared_bytes"), 1048576) + shin::ui::tr(" MiB")});
            out.push_back({shin::ui::tr("Automatic cleanup: ") + std::string(field(value, "auto_clean") == true ? shin::ui::tr("on") : shin::ui::tr("off"))});
            out.push_back({});
            const auto& rows = field(value, "projects");
            if (rows.is_array()) for (const auto& row : rows) {
                out.push_back({text(field(row, "root")), Tone::heading});
                out.push_back({"  " + number(field(row, "used_bytes"), 1048576) + shin::ui::tr(" MiB | Cache: ") + number(field(row, "cache_bytes"), 1048576) + shin::ui::tr(" MiB | ") + shin::ui::tr(text(field(row, "reason")).c_str())});
            }
        }
        out.push_back({}); out.push_back({shin::ui::tr("Inventory is a point-in-time read. r refreshes it; no automatic rescans.")});
        out.push_back({shin::ui::tr("For cleanup, review shin-ce storage-status and its revision in a separate command.")});
    } else {
        out.push_back({shin::ui::tr("Model usage"), Tone::heading});
        const auto& usage = snapshot.usage;
        if (field(usage, "status") != "available") {
            out.push_back({std::string(shin::ui::tr("Usage: ")) + shin::ui::tr(text(field(usage, "status")).c_str()), Tone::warning});
            out.push_back({shin::ui::tr("Pass --usage-file /absolute/path/to/usage/state.json to read collected counters.")});
            out.push_back({shin::ui::tr("This screen does not scan client sessions or start a collector.")});
        } else {
            if (field(usage, "fresh") != true || field(usage, "complete") != true)
                out.push_back({shin::ui::tr("Usage is stale or incomplete; totals are the available observations."), Tone::warning});
            out.push_back({shin::ui::tr("Unassigned sessions: ") + number(field(usage, "unassigned_sessions")) + shin::ui::tr(" | Skipped: ") + number(field(usage, "skipped_sessions"))});
            const auto& groups = field(usage, "projects");
            if (groups.is_array() && !groups.empty()) {
                state.usage_project %= groups.size();
                const auto& group = groups[state.usage_project];
                out.push_back({field(group, "id") == "" ? shin::ui::tr("All usage projects") : text(field(group, "name")), Tone::heading});
                const auto& totals = field(group, "totals");
                for (const auto* item : {"input_tokens", "output_tokens", "cache_read_tokens", "cache_write_tokens", "reasoning_tokens"})
                    out.push_back({std::string(shin::ui::tr(item)) + ": " + number(field(totals, item))});
                out.push_back({});
                const auto& models = field(group, "models");
                if (models.is_array()) for (const auto& row : models)
                    out.push_back({text(field(row, "client")) + " / " + text(field(row, "model")) + shin::ui::tr(" | in ") + number(field(row, "input_tokens")) + shin::ui::tr(" | out ") + number(field(row, "output_tokens"))});
            }
            out.push_back({}); out.push_back({shin::ui::tr("p next usage project  |  Counters use the collector's configured date range.")});
        }
    }
    return out;
}
} // namespace shin::observer::tui
