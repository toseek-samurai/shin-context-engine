#pragma once
#include "shin/observer/local_project.h"
#include <algorithm>
#include <chrono>
#include <set>
#include <vector>

namespace shin::observer {
// Registry admission is serial. A healthy multi-project startup may exceed one
// minute in total; only first admission/settlement of a selected root is progress.
class LocalStartupAdmission {
public:
    using Clock = std::chrono::steady_clock;
    LocalStartupAdmission(std::vector<fs::path> roots, std::size_t resident_limit,
                          Clock::time_point now = Clock::now())
        : roots_(std::move(roots)), resident_limit_(resident_limit),
          hard_deadline_(now + std::chrono::minutes(5)), deadline_(now + std::chrono::seconds(60)) {}

    Clock::time_point deadline() const { return deadline_; }
    bool expired(Clock::time_point now = Clock::now()) const { return now >= deadline_; }
    static void timeout() {
        throw Error("service-enrollment-timeout",
            "Registered project preparation did not settle within the bounded startup wait; inspect service-status for pending projects.");
    }
    bool observe(const Json& status, Clock::time_point now = Clock::now()) {
        if (expired(now)) timeout();
        const auto instance = status.at("instance_id").get<std::string>();
        if (instance_.empty()) instance_ = instance;
        else if (instance_ != instance)
            throw Error("service-start-interrupted", "The service instance changed during startup; inspect service-status before retrying.");
        bool progress = false, complete = true;
        for (const auto& root : roots_) {
            const auto name = root.string();
            const auto& projects = status.at("projects");
            if (std::find(projects.begin(), projects.end(), Json(name)) != projects.end())
                progress |= admitted_.insert(name).second;
            const auto& readers = status.at("resident_projects");
            const bool ready = std::any_of(readers.begin(), readers.end(), [&](const Json& row) {
                return row.at("root") == name && row.at("ready") == true;
            });
            const auto& qualified = status.at("qualified_projects");
            const bool standby = (roots_.size() > resident_limit_ || status.value("resident_memory_limited", false)) &&
                std::any_of(qualified.begin(), qualified.end(), [&](const Json& row) { return row.at("root") == name; });
            const bool settled = ready || standby || status.at("enrollment_errors").contains(name);
            if (settled) progress |= settled_.insert(name).second;
            complete &= settled;
        }
        if (progress) deadline_ = std::min(hard_deadline_, now + std::chrono::seconds(60));
        return complete;
    }
private:
    std::vector<fs::path> roots_;
    std::size_t resident_limit_;
    Clock::time_point hard_deadline_, deadline_;
    std::set<std::string> admitted_, settled_;
    std::string instance_;
};
} // namespace shin::observer
