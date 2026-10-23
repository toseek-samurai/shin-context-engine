#include "../src/local_startup.h"
#include <iostream>
using namespace shin::observer;
namespace {
using Clock = LocalStartupAdmission::Clock;
const auto start = Clock::time_point{};
auto at(int seconds) { return start + std::chrono::seconds(seconds); }
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<fs::path> roots(int count) {
    std::vector<fs::path> value;
    for (int i = 0; i < count; ++i) value.emplace_back("/project" + std::to_string(i));
    return value;
}
Json status() {
    return {{"instance_id", "one-instance"}, {"projects", Json::array()},
        {"resident_projects", Json::array()}, {"qualified_projects", Json::array()},
        {"enrollment_errors", Json::object()}};
}
void timed_out(LocalStartupAdmission& admission, const Json& value, int seconds) {
    bool refused = false;
    try { (void)admission.observe(value, at(seconds)); }
    catch (const Error& error) { refused = error.code == "service-enrollment-timeout"; }
    check(refused, "startup did not report its own timeout");
}
}
int main() try {
    // Reproduce serial successful admission crossing the old global 60 seconds.
    auto names = roots(10);
    auto value = status();
    LocalStartupAdmission admission(names, 5, start);
    check(!admission.observe(value, at(0)), "empty service certified registry readiness");
    const int times[]{13, 20, 30, 33, 35, 40, 45, 57, 59, 62};
    for (int i = 0; i < 10; ++i) {
        value["projects"].push_back(names[i].string());
        check(!admission.observe(value, at(times[i])), "admission alone certified readiness");
    }
    for (const auto& root : names)
        value["qualified_projects"].push_back({{"root", root.string()}, {"snapshot_id", "fixture"}});
    check(admission.observe(value, at(65)), "healthy multi-project startup was cancelled at 60 seconds");

    // A changed phase/snapshot or an unrelated project is not new admission.
    value = status();
    value["projects"].push_back(names[0].string());
    value["qualified_projects"].push_back({{"root", names[0].string()}, {"snapshot_id", "first"}});
    LocalStartupAdmission stalled(names, 5, start);
    check(!stalled.observe(value, at(0)), "partial registry was ready");
    const auto deadline = stalled.deadline();
    value["phase"] = "checking";
    value["qualified_projects"][0]["snapshot_id"] = "changed";
    value["projects"].push_back("/unrelated");
    value["qualified_projects"].push_back({{"root", "/unrelated"}, {"snapshot_id", "changed"}});
    check(!stalled.observe(value, at(59)) && stalled.deadline() == deadline, "status polling extended startup");
    timed_out(stalled, value, 60);
    value["projects"].push_back(names[1].string());
    timed_out(stalled, value, 61); // Late progress cannot revive an expired operation.

    // Finite first-time milestones still cannot defeat the absolute cap.
    names = roots(16);
    value = status();
    LocalStartupAdmission bounded(names, 5, start);
    check(!bounded.observe(value, at(0)), "empty registry was ready");
    for (int i = 0; i < 14; ++i) {
        value["projects"].push_back(names[i].string());
        check(!bounded.observe(value, at(20 * (i + 1))), "unfinished registry was ready");
    }
    check(bounded.deadline() == at(300), "startup escaped total bound");
    timed_out(bounded, value, 300);

    // Preserve resident/standby/memory and explicit failure semantics.
    names = roots(1);
    value = status();
    value["projects"].push_back(names[0].string());
    value["qualified_projects"].push_back({{"root", names[0].string()}});
    LocalStartupAdmission resident(names, 5, start);
    check(!resident.observe(value, at(0)), "cold reader was certified on a small registry");
    value["resident_memory_limited"] = true;
    check(resident.observe(value, at(1)), "memory-qualified standby was rejected");
    value.erase("resident_memory_limited");
    value["resident_projects"].push_back({{"root", names[0].string()}, {"ready", false}});
    LocalStartupAdmission warming(names, 5, start);
    check(!warming.observe(value, at(0)), "unready resident was certified");
    value["resident_projects"][0]["ready"] = true;
    check(warming.observe(value, at(1)), "ready resident was rejected");
    value = status();
    value["enrollment_errors"][names[0].string()] = "registry-binding-changed";
    LocalStartupAdmission failed(names, 5, start);
    check(failed.observe(value, at(0)) && !value["enrollment_errors"].empty(), "enrollment failure was hidden or retried");
    value = status();
    LocalStartupAdmission replaced(names, 5, start);
    check(!replaced.observe(value, at(0)), "empty service was ready");
    value["instance_id"] = "other-instance";
    bool interrupted = false;
    try { (void)replaced.observe(value, at(1)); }
    catch (const Error& error) { interrupted = error.code == "service-start-interrupted"; }
    check(interrupted, "replacement service inherited startup progress");
    std::cout << "Serial startup progress, idle/total bounds, instance and readiness safeguards passed\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
