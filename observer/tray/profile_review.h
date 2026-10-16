#pragma once

#include "shin/observer/local_project.h"
#include "shin/observer/local_installation.h"
#include <deque>
#include <chrono>
#include <mutex>

namespace {
using namespace shin::observer;
struct ProfileReviewAction {
    enum class Kind { preview, accept } kind = Kind::preview;
    std::string root;
    std::string revision;
    std::string token;
};

struct ProfileReviewSnapshot {
    std::string root;
    std::string status = "idle";
    std::string error;
    Json review;
    bool partial = false;
    std::uint64_t partial_files = 0;
    Json partial_details = Json::array();
    std::uint64_t baseline_failures = 0;
    std::optional<double> baseline_success_age_ms;
    bool baseline_success_age_available = false;
    std::chrono::steady_clock::time_point accepted_at{};
};

// UI actions enqueue immutable root/revision/token tuples. The Monitor worker
// is the only consumer and publisher, so no project operation runs on render.
class ProfileReviewChannel {
public:
    bool preview(const std::string& root) {
        if (root.empty() || root.size() > 4096) return false;
        std::lock_guard lock(mutex_);
        if (snapshot_.root == root && (snapshot_.status == "loading" || snapshot_.status == "accepting" ||
            snapshot_.status == "accepted-waiting")) return false;
        if (!snapshot_.root.empty() && snapshot_.root != root &&
            (snapshot_.status == "loading" || snapshot_.status == "accepting" ||
             snapshot_.status == "accepted-waiting")) return false;
        snapshot_ = ProfileReviewSnapshot{};
        snapshot_.root = root;
        snapshot_.status = "loading";
        snapshot_.review = Json::object();
        actions_.push_back({ProfileReviewAction::Kind::preview, root, {}, {}});
        return true;
    }

    bool accept() {
        std::lock_guard lock(mutex_);
        if (snapshot_.status != "ready" || !snapshot_.review.is_object() ||
            !snapshot_.review.value("can_accept", false)) return false;
        const auto revision = snapshot_.review.value("revision", std::string{});
        const auto token = snapshot_.review.value("review_token", std::string{});
        if (revision.empty() || token.empty()) return false;
        snapshot_.status = "accepting";
        actions_.push_back({ProfileReviewAction::Kind::accept, snapshot_.root, revision, token});
        return true;
    }

    std::optional<ProfileReviewAction> take() {
        std::lock_guard lock(mutex_);
        if (actions_.empty()) return std::nullopt;
        auto action = std::move(actions_.front());
        actions_.pop_front();
        return action;
    }

    ProfileReviewSnapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

    void preview_ready(const std::string& root, Json review) {
        std::lock_guard lock(mutex_);
        if (snapshot_.root != root || snapshot_.status != "loading") return;
        snapshot_.review = std::move(review);
        snapshot_.status = snapshot_.review.value("changed", false) ? "ready" : "unchanged";
    }

    void failed(const std::string& root, std::string code) {
        std::lock_guard lock(mutex_);
        if (snapshot_.root != root) return;
        snapshot_.status = code == "profile-review-changed" || code == "profile-revision-changed"
            ? "stale" : "error";
        snapshot_.error = std::move(code);
    }

    void accepted(const std::string& root, std::optional<double> success_age_ms, bool success_age_available,
                  std::uint64_t failures) {
        std::lock_guard lock(mutex_);
        if (snapshot_.root == root && snapshot_.status == "accepting") {
            snapshot_.status = "accepted-waiting";
            snapshot_.baseline_failures = failures;
            snapshot_.baseline_success_age_ms = success_age_ms;
            snapshot_.baseline_success_age_available = success_age_available;
            snapshot_.accepted_at = std::chrono::steady_clock::now();
        }
    }

    void observed(const std::string& root, std::optional<double> success_age_ms, bool success_age_available,
                  std::uint64_t failures,
                  bool still_conflicted, bool active_issue, const std::string& phase,
                  bool partial, std::uint64_t partial_files = 0,
                  Json partial_details = Json::array()) {
        std::lock_guard lock(mutex_);
        if (snapshot_.root != root || snapshot_.status != "accepted-waiting") return;
        const bool newly_failed = failures > snapshot_.baseline_failures;
        if (newly_failed && active_issue) {
            snapshot_.status = "failed";
            return;
        }
        if (still_conflicted) return;
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - snapshot_.accepted_at).count();
        const bool new_success = success_age_available && success_age_ms && phase == "current" && !active_issue &&
            snapshot_.baseline_success_age_available &&
            *success_age_ms <= elapsed + 250.0 &&
            (!snapshot_.baseline_success_age_ms || *success_age_ms + 1000.0 < *snapshot_.baseline_success_age_ms + elapsed);
        if (new_success) {
            snapshot_.status = partial ? "partial" : "success";
            snapshot_.partial = partial;
            snapshot_.partial_files = partial_files;
            snapshot_.partial_details = std::move(partial_details);
        }
    }

private:
    mutable std::mutex mutex_;
    std::deque<ProfileReviewAction> actions_;
    ProfileReviewSnapshot snapshot_;
};

inline Json request_profile_review(const fs::path& home, const ProfileReviewAction& action) {
    const auto executable = current_executable_path();
    auto installation = lock_local_installation(executable);
    auto selected_home = home;
    if (const auto installed = installed_local_home(executable)) {
        if (fs::absolute(home).lexically_normal() != *installed)
            throw Error("installation-home-conflict", "The selected home differs from this installation.");
        selected_home = *installed;
    }
    check_local_installation_home(selected_home, executable);
    LocalProjectRequest request;
    request.home = selected_home;
    request.root = fs::path(action.root);
    request.command = action.kind == ProfileReviewAction::Kind::preview ? "profile-review" : "profile-accept";
    request.profile_revision = action.revision;
    request.profile_review_token = action.token;
    return local_project(request);
}

inline bool validate_profile_review(const Json& result) {
    if (!result.is_object() || result.value("ok", false) != true || !result.contains("profile_review")) return false;
    const auto& review = result.at("profile_review");
    if (!review.is_object() || review.value("schema_version", std::string{}) != "shin.profile-review.v1" ||
        !review.value("revision", std::string{}).size() || !review.value("review_token", std::string{}).size() ||
        !review.contains("changed") || !review.at("changed").is_boolean() ||
        !review.contains("can_accept") || !review.at("can_accept").is_boolean() ||
        !review.contains("baseline_available") || !review.at("baseline_available").is_boolean() ||
        !review.contains("checks_preserved") || !review.at("checks_preserved").is_boolean() ||
        !review.contains("categories") || !review.at("categories").is_array() || review.at("categories").size() > 16 ||
        !review.contains("changed_fields") || !review.at("changed_fields").is_array() || review.at("changed_fields").size() > 128)
        return false;
    for (const auto& category : review.at("categories")) {
        if (!category.is_object() || !category.value("id", std::string{}).size()) return false;
        for (const auto* key : {"added", "removed", "changed"})
            if (!category.contains(key) || !category.at(key).is_number_integer() || category.at(key).get<std::int64_t>() < 0)
                return false;
    }
    for (const auto& field : review.at("changed_fields")) if (!field.is_string() || field.get<std::string>().size() > 160) return false;
    return true;
}
}
