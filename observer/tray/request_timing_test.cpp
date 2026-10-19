#include "../ui/request_timing.h"
#include "../ui/service_activity.h"
#include <cassert>
#include <iostream>

int main() try {
    using nlohmann::json;
    using shin::ui::request_timing;
    assert(shin::ui::service_activity_lines(json::object()).empty());
    json activity{{"activity", {{"schema_version","shin.service-activity.v1"},
        {"calls",{{"query",2},{"inspect",1},{"read",3},{"status",4}}},
        {"diagnostic",1},{"uncorrelated",2},{"reported_sessions",64},{"sessions_truncated",true}}}};
    auto lines = shin::ui::service_activity_lines(activity);
    assert(lines.size() == 5 && lines[3].find("64+") != std::string::npos);
    activity["activity"]["models"] = {{"fixture-model", std::uint64_t{2}}};
    activity["activity"]["model_unknown"] = 3;
    activity["activity"]["model_overflow"] = 1;
    lines = shin::ui::service_activity_lines(activity);
    assert(lines.size() == 5 && lines[4].find("fixture-model 2") != std::string::npos);
    assert(lines[4].find("unknown: 3") != std::string::npos);
    activity["activity"]["calls"]["query"] = -1;
    assert(shin::ui::service_activity_lines(activity).empty());
    activity["activity"]["calls"]["query"] = "2";
    assert(shin::ui::service_activity_lines(activity).empty());
    const auto cohort = [](int requests, double worker, int measured, const char* purpose, const char* reader) {
        return json::array({"root", "query", purpose, reader, requests, 0, measured, worker + 100, worker + 100,
            worker, worker, nullptr, worker / 2, worker / 2, 2, nullptr, measured, 0, measured, measured, measured, 0});
    };
    json data{{"history", {{"status", "ready"}, {"request_cohorts", json::array({
        cohort(100, 10, 100, "unclassified", "warm"), cohort(10, 100, 1, "diagnostic", "cold")})}}}};
    auto result = request_timing(data, true, 0);
    assert(result.requests == 110 && result.worker.count == 101);
    assert(std::abs(result.worker.value().value_or(-1) - 1100.0 / 101) < .001);
    assert(result.phases[0].count == 0 && !result.phases[0].value());
    assert(result.phases[1].count == 101 && result.readers[1].requests == 10);
    assert(request_timing(data, true, 1).requests == 100);
    assert(request_timing(data, true, 2).requests == 10);
    data["history"]["request_cohorts_truncated"] = true;
    assert(request_timing(data, true, 0).truncated);
    data["history"]["request_cohorts"][0][16] = 101;
    assert(request_timing(data, true, 0).invalid);
    assert(request_timing(data, true, 0).requests == 10);
    data["history"]["status"] = "unavailable";
    assert(!request_timing(data, true, 0).available);
    assert(!request_timing(json::object(), true, 0).available);
    data["request_metrics"]["recent"] = json::array({
        {{"method", "query"}, {"outcome", "response-ready"}, {"duration_ms", 25}, {"service_duration_ms", 150},
         {"reader_state", "warm"}, {"purpose", "diagnostic"}, {"phase_ms", {{"source", 15}, {"admission", -1}}}},
        {{"method", "query"}, {"duration_ms", nullptr}}, {{"method", "inspect"}, {"duration_ms", 900}}
    });
    result = request_timing(data, false, 0);
    assert(result.requests == 2 && result.worker.count == 1 && result.worker.value() == 25);
    assert(result.service.count == 1 && result.readers[2].requests == 1);
    assert(!result.phases[0].value() && result.phases[2].value() == 15);
    assert(request_timing(data, false, 1).requests == 1);
    assert(!request_timing(data, false, 0).truncated);
    data["request_metrics"]["recent_truncated"] = true;
    assert(request_timing(data, false, 0).truncated);
    std::cout << "weighted timing, missing data, filters and bounds passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
