#include "shin/server/publication_service.h"
#include "shin/server/candidate_artifact.h"
#include "snapshot_verification.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace shin::context {
VerifiedSnapshot load_verified_snapshot_with_review_barrier(const fs::path& root,
    std::uintmax_t maximum_payload_bytes, std::uintmax_t maximum_file_bytes);
}

namespace {
using namespace shin::server;
namespace fs = std::filesystem;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Barrier {
    std::mutex mutex;
    std::condition_variable changed;
    fs::path target;
    bool entered = false;
    bool released = false;

    void before_load(const fs::path& root) {
        if (root != target) return;
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        check(changed.wait_for(lock, std::chrono::seconds(20), [&] { return released; }), "review barrier timed out");
    }
    void wait() {
        std::unique_lock lock(mutex);
        check(changed.wait_for(lock, std::chrono::seconds(20), [&] { return entered; }), "review never reached second artifact");
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};
Barrier barrier;

Json run(const fs::path& config_path, std::string_view id_text) {
    check(config_path.is_absolute() && fs::canonical(config_path) == config_path &&
          config_path.filename() == "runtime.json" && config_path.parent_path().filename() == "config",
          "probe requires the explicit development runtime config");
    std::uint64_t id = 0;
    const auto parsed = std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);
    check(parsed.ec == std::errc{} && parsed.ptr == id_text.data() + id_text.size() && id > 0 &&
          id <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), "invalid proposal id");
    const auto config = load_config(config_path);
    check(config.database.expected_database == "shin_server_dev" && config.database.runtime_role == "shin_runtime" &&
          config.database.source_host_id == "local-dev", "probe refuses non-development database");
    const RouteCatalog catalog(config);
    const auto& source = catalog.find("visible", "visible-retrieval");
    check(fs::path(source.route.root) == config_path.parent_path().parent_path() / "fixtures" / "roots" / "visible",
          "probe requires the named local fixture source");
    const auto admin = std::find_if(config.credentials.begin(), config.credentials.end(),
                                   [](const auto& entry) { return entry.principal.admin; });
    check(admin != config.credentials.end(), "missing configured administrator");
    const auto& principal = admin->principal;
    Store store(config.database);
    PreparationService preparations(config, catalog, store);
    PublicationService publications(config, catalog, store, preparations);
    const auto detail = store.publication_proposal(principal, "visible", "review-race", id);
    const auto candidate = detail.at("candidate").at("preparation_id").get<std::uint64_t>();
    const auto current = detail.at("current").at("preparation_id").get<std::uint64_t>();
    check(detail.at("base").at("preparation_id") == candidate && candidate != current &&
          detail.at("proposal").at("state") == "rejected", "probe requires a closed base=candidate historical proposal");
    const auto plan = preparations.retention_plan(principal, candidate);
    check(plan.at("can_apply") == true, "historical candidate must initially be eligible for retention");
    const auto selected = store.preparation_inspection_input(principal, current);
    barrier.target = config.preparations.state_dir / candidate_artifact_name(current, selected.at("artifact_key").get<std::string>());
    auto reviewing = std::async(std::launch::async, [&] { return publications.review(principal, "visible", "review-race", id); });
    Json review;
    std::string refusal;
    std::string history_refusal;
    try {
        barrier.wait();
        try {
            (void)publications.retire_history(principal,
                {{"plan", store.plan_publication_retention(principal, 64)}, {"discard_confirmed", true}});
        } catch (const ServiceError& error) { history_refusal = error.code; }
        try {
            (void)preparations.apply_retention(principal, candidate, {{"expected_plan_id", plan.at("plan_id")}});
        } catch (const ServiceError& error) { refusal = error.code; }
        barrier.release();
        review = reviewing.get();
    } catch (...) {
        barrier.release();
        if (reviewing.valid()) reviewing.wait();
        throw;
    }
    check(refusal == "candidate-artifact-busy", "review failed to exclude retention of its first artifact");
    check(history_refusal == "publication-review-busy", "admitted review did not exclude history retirement");
    for (const auto* side : {"base", "current", "candidate"})
        check(review.at("review").at("artifacts").at(side).at("state") == "artifact-verified", "review returned contradictory artifact state");
    check(review.at("review").at("comparisons").at("base").at("counts") == Json{{"added", 0}, {"removed", 0}, {"changed", 0}},
          "same base/candidate comparison was not empty");
    check(store.publication_proposal(principal, "visible", "review-race", id) == detail &&
          store.preparation_inspection_input(principal, candidate).at("retention").is_null(), "review race changed durable state");
    check(preparations.retention_plan(principal, candidate).at("can_apply") == true,
          "review kept a lifetime lock after returning");
    return {{"ok", true}, {"retention_error", refusal}, {"history_error", history_refusal},
            {"all_artifacts_verified", true}, {"locks_released", true}};
}
} // namespace

namespace shin::context {
VerifiedSnapshot load_verified_snapshot_with_review_barrier(const fs::path& root,
    std::uintmax_t maximum_payload_bytes, std::uintmax_t maximum_file_bytes) {
    barrier.before_load(root);
    return load_verified_snapshot(root, maximum_payload_bytes, maximum_file_bytes);
}
}

int main(int argc, char** argv) {
    if (argc != 3) { std::cout << "{\"ok\":false}\n"; return 2; }
    try { std::cout << run(fs::path(argv[1]), argv[2]).dump() << '\n'; return 0; }
    catch (...) { std::cout << "{\"ok\":false}\n"; return 1; }
}
