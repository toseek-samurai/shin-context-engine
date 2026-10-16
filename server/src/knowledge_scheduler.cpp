#include "shin/server/knowledge_scheduler.h"
#include <algorithm>

namespace shin::server {
namespace {
std::int64_t epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
Json unavailable(std::string_view reason) {
    return {{"healthy", false}, {"assessment_complete", false}, {"generation", nullptr},
            {"reason_codes", Json::array({reason})}, {"mutations_performed", nullptr}};
}
}

KnowledgeScheduler::KnowledgeScheduler(KnowledgeConfig policy, std::vector<KnowledgeTarget> targets, Inspector inspect)
    : policy_(std::move(policy)), inspect_(std::move(inspect)) {
    if (targets.size() > 8 || policy_.interval_seconds < 1 || policy_.max_age_seconds < policy_.interval_seconds)
        throw ServiceError(400, "invalid-knowledge-policy", "Knowledge scheduler bounds are invalid.");
    for (auto& target : targets) entries_.push_back(Entry{.target = std::move(target)});
}

KnowledgeScheduler::~KnowledgeScheduler() {
    request_stop();
    if (worker_.joinable()) worker_.join();
}

void KnowledgeScheduler::request_stop() {
    worker_.request_stop();
    wake_.notify_all();
}

void KnowledgeScheduler::start() {
    if (worker_.joinable()) throw ServiceError(500, "knowledge-already-started", "Knowledge worker is already running.");
    if (policy_.enabled) worker_ = std::jthread([this](std::stop_token stop) { work(stop); });
}

Json KnowledgeScheduler::refresh() {
    std::lock_guard lock(mutex_);
    if (!policy_.enabled) throw ServiceError(409, "knowledge-disabled", "Knowledge maintenance is disabled.");
    ++refreshes_;
    const auto now = Clock::now();
    for (auto& entry : entries_) {
        if (entry.checking) {
            if (entry.recheck) ++coalesced_;
            entry.recheck = true;
        } else {
            if (entry.due <= now) ++coalesced_;
            entry.due = now;
        }
    }
    wake_.notify_all();
    return {{"requested", true}, {"targets", entries_.size()}, {"completed_synchronously", false}};
}

Json KnowledgeScheduler::status() const {
    std::lock_guard lock(mutex_);
    const auto active = std::find_if(entries_.begin(), entries_.end(), [](const Entry& e) { return e.checking; });
    return {{"schema_version", "shin.knowledge-maintenance.v1"}, {"enabled", policy_.enabled},
        {"state", !policy_.enabled ? "disabled" : active == entries_.end() ? "idle" : "checking"},
        {"active_route", active == entries_.end() ? Json(nullptr) : Json(active->target.route_id)},
        {"targets", entries_.size()}, {"checks_completed", completed_}, {"checks_failed", failed_},
        {"checks_deferred", deferred_}, {"refresh_requests", refreshes_}, {"coalesced_requests", coalesced_},
        {"read_only", true}, {"scope", "process-local"}};
}

Json KnowledgeScheduler::snapshot(const std::string& route) const {
    std::lock_guard lock(mutex_);
    Json result{{"schema_version", "shin.knowledge-health.v1"}, {"route_id", route}, {"state", "not-monitored"},
        {"assessment", nullptr}, {"age_ms", nullptr}, {"max_age_ms", policy_.max_age_seconds * 1000LL},
        {"observed_at_unix_ms", nullptr}, {"completed_at_unix_ms", nullptr}, {"elapsed_ms", nullptr},
        {"checking", false}, {"read_only", true}, {"exact_current_at_read", false}};
    if (!policy_.enabled) { result["state"] = "disabled"; return result; }
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.target.route_id == route; });
    if (found == entries_.end()) return result;
    result["checking"] = found->checking;
    if (found->report.is_null()) { result["state"] = "pending"; return result; }
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - found->observed).count();
    result["state"] = age >= policy_.max_age_seconds * 1000LL ? "expired" :
        !found->report.value("assessment_complete", true) ? "unavailable" : found->report.at("healthy") == true ? "healthy" : "unhealthy";
    result["assessment"] = found->report;
    result["age_ms"] = age;
    result["observed_at_unix_ms"] = found->started_at;
    result["completed_at_unix_ms"] = found->finished_at;
    result["elapsed_ms"] = found->elapsed_ms;
    return result;
}

void KnowledgeScheduler::work(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        auto next = std::min_element(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.due < b.due; });
        if (next == entries_.end()) return;
        const auto now = Clock::now();
        if (next->due > now) {
            const auto deadline = next->due;
            wake_.wait_until(lock, stop, deadline, [&] {
                return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& entry) { return entry.due < deadline; });
            });
            continue;
        }
        next->checking = true;
        const auto started_at = epoch_ms();
        lock.unlock();
        Json report;
        bool deferred = false;
        try {
            report = inspect_(next->target);
            if (!report.is_object() || !report.contains("healthy") || !report["healthy"].is_boolean() || report.dump().size() > 16384)
                throw ServiceError(500, "knowledge-report-invalid", "Knowledge assessment exceeded its contract.");
        } catch (const ServiceError& error) {
            deferred = error.code == "knowledge-foreground-busy";
            report = unavailable(error.code);
        } catch (...) { report = unavailable("knowledge-assessment-unavailable"); }
        const auto ended = Clock::now();
        lock.lock();
        next->checking = false;
        if (deferred) ++deferred_;
        else {
            next->report = std::move(report);
            next->observed = now;
            next->started_at = started_at;
            next->finished_at = epoch_ms();
            next->elapsed_ms = std::chrono::duration<double, std::milli>(ended - now).count();
            ++completed_;
            if (next->report.at("healthy") != true) ++failed_;
        }
        next->due = next->recheck ? ended : ended + std::chrono::seconds(deferred ? 1 : policy_.interval_seconds);
        next->recheck = false;
    }
}

} // namespace shin::server
