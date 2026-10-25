#include "../src/local_session_analysis.h"
#include "shin/observer/observer.h"
#include "../ui/session_activity.h"
#include <chrono>
#include <iostream>
using namespace shin::observer;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
const std::string session(64,'a'), project(64,'b'), file(64,'c'), version(64,'d');
Json call(std::int64_t at, const char* method = "read") {
    return {{"at_ms",at},{"method",method},{"outcome","success"},{"write_state","written"},
        {"purpose","unclassified"},{"project_id",project},{"read_file_id",file},{"read_version_id",version}};
}
Json marker(std::int64_t at, const char* kind) {
    return {{"at_ms",at},{"kind",kind},{"session_id",session},{"event_id",digest(std::to_string(at)+kind)}};
}
Json history(Json events) {
    return {{"fresh",true},{"updated_at_ms",600},{"context_events",{{"schema_version","shin.context-events.v1"},
        {"source","codex"},{"complete",true},{"events",std::move(events)}}}};
}
}
int main() try {
    auto usage = history(Json::array({marker(100,"turn-start"),marker(200,"compaction"),marker(300,"turn-finish"),
        marker(400,"turn-start"),marker(500,"turn-finish")}));
    Json calls = Json::array({call(90),call(110),call(120),call(130),call(140),call(150),call(160),
        call(210),call(220),call(230),call(240),call(250,"inspect"),call(260,"query"),call(270,"resume"),call(300),call(410)});
    calls[3]["read_version_id"] = std::string(64,'e');
    calls[4]["purpose"] = "diagnostic";
    calls[5]["outcome"] = "tool-error";
    calls[6]["write_state"] = "failed";
    calls[9]["project_id"] = std::string(64,'f');
    calls[10].erase("read_file_id");
    auto result = analyze_session_activity(calls,usage,session,0,600,true);
    const auto& reads = result.at("reads");
    check(reads.at("reads") == 10 && reads.at("unknown_reads") == 1,"failed/unwritten/probe reads entered the cohort");
    check(reads.at("files") == 2 && reads.at("versions") == 3 && reads.at("same_version_repeats") == 6 &&
        reads.at("additional_versions") == 1,"same-version/project/version identity was merged incorrectly");
    const auto& episodes = result.at("episodes");
    check(episodes.size() == 3 && result.at("unassigned_context_calls") == 2,"explicit intervals or boundary ambiguity lost");
    check(episodes[0].at("reads").at("previously_seen_versions") == 1,"cross-episode revisit absent");
    check(episodes[1].at("kind") == "compaction" && episodes[1].at("reads").at("same_version_repeats") == 1 &&
        episodes[1].at("reads").at("previously_seen_versions") == 1,"within-episode and earlier-version reads conflated");
    check(episodes[2].at("diagnostic_calls") == 1 && episodes[2].at("errors") == 1,"probe/error semantics lost");
    check(episodes[1].at("interval_complete") == false,"unknown read identity presented as complete repeat analysis");
    check(episodes[2].at("interval_complete") == true,"complete closed interval missing");
    auto gaps = analyze_session_activity(calls,usage,session,115,600,false);
    check(gaps.at("episodes")[2].at("interval_complete") == false && gaps.at("reads").at("reads") == 8,"retention/gaps invented complete coverage");
    auto pending = calls; pending[0]["outcome"] = "unfinished"; pending[0]["write_state"] = "unobserved";
    check(analyze_session_activity(pending,usage,session,0,600,true).at("calls_complete") == false,"unfinished observations presented as complete");
    auto codex_only = usage; codex_only["fresh"] = false;
    codex_only["sources"] = {{"codex",{{"state","available"},{"last_success_unix_ms",600}}}};
    check(analyze_session_activity(calls,codex_only,session,0,600,true).at("host_markers_complete") == true,
        "an unavailable unrelated token client suppressed current Codex boundaries");
    codex_only["collection_error"] = "collection-failed";
    check(analyze_session_activity(calls,codex_only,session,0,600,true).at("host_markers_complete") == false,"collection health error ignored");
    auto missing = analyze_session_activity(calls,Json(),session,0,600,true);
    check(missing.at("episodes").empty() && missing.at("reads") == reads,"missing host history destroyed counts or invented episodes");
    auto stale = usage; stale["fresh"] = false;
    check(analyze_session_activity(calls,stale,session,0,600,true).at("episodes")[0].at("interval_complete") == false,"stale marker coverage accepted");
    auto other = usage; for (auto& event : other["context_events"]["events"]) event["session_id"] = std::string(64,'f');
    check(analyze_session_activity(calls,other,session,0,600,true).at("episodes").empty(),"foreign session markers were joined");
    auto duplicates = usage; duplicates["context_events"]["events"].push_back(marker(200,"compaction"));
    check(analyze_session_activity(calls,duplicates,session,0,600,true) == result,"same event counted twice");
    auto tied = history(Json::array({marker(100,"turn-start"),marker(100,"compaction"),marker(300,"turn-finish")}));
    check(analyze_session_activity(calls,tied,session,0,600,true).at("episodes").empty(),"tied boundaries assigned arbitrary order");
    auto conflict = history(Json::array({marker(100,"turn-start"),marker(300,"turn-finish")}));
    conflict["context_events"]["events"][0]["turn_id"] = std::string(64,'a');
    conflict["context_events"]["events"][1]["turn_id"] = std::string(64,'b');
    check(analyze_session_activity(calls,conflict,session,0,600,true).at("episodes").empty(),"conflicting turn identities were merged");
    auto boundary_read = analyze_session_activity(Json::array({call(100),call(110)}),
        history(Json::array({marker(100,"turn-start"),marker(300,"turn-finish")})),session,0,600,true);
    check(boundary_read.at("episodes")[0].at("reads").at("previously_seen_versions") == 0,
        "an exactly-on-boundary read was treated as a known earlier read");
    auto open = history(Json::array({marker(100,"turn-start")})); open["updated_at_ms"] = 200;
    const auto partial = analyze_session_activity(calls,open,session,0,600,true);
    check(partial.at("episodes")[0].at("end_ms") == 200 && partial.at("episodes")[0].at("closed") == false &&
        partial.at("episodes")[0].at("context_calls") == 5,"calls after last host collection assigned to a guessed task");
    auto many = Json::array(); for (int i = 0; i < 5; ++i) { many.push_back(marker(100+i*50,"turn-start")); }
    check(analyze_session_activity(calls,history(many),session,0,600,true).at("episodes_truncated") == true,"episode output bound missing");
    Json telemetry{{"mcp_activity",{{"status","ready"},{"session_rows",Json::array({{{"session_id",session},{"analysis",result}}})}}}};
    const auto join = [](const auto& lines) { std::string output; for (const auto& line : lines) output += line+'\n'; return output; };
    auto rendered = join(shin::ui::session_activity_lines(telemetry));
    check(rendered.find("Same version again: 6") != std::string::npos && rendered.find("After host compaction") != std::string::npos,"shared UI projection missing");
    check(rendered.find(file) == std::string::npos && rendered.find(version) == std::string::npos,"file identity leaked into UI");
    shin::ui::set_language(shin::ui::Language::ru);
    check(join(shin::ui::session_activity_lines(telemetry)).find("Повтор той же версии: 6") != std::string::npos,"RU session analysis absent");
    shin::ui::set_language(shin::ui::Language::en);
    Json full = Json::array(); for (int i = 0; i < 10000; ++i) full.push_back(call(101));
    const auto start = std::chrono::steady_clock::now();
    auto bounded = analyze_session_activity(full,usage,session,0,600,true);
    check(bounded.at("reads").at("same_version_repeats") == 9999 && bounded.dump().size() < 4096,"retained-row scale changed counts or output bound");
    std::cout << "Explicit episodes, repeat versions, unknowns, gaps, probes, cross-project isolation, UTC/RU UI passed; 10000 rows in "
        << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count() << " ms\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
