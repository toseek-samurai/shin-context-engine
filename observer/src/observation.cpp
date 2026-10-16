#include "shin/observer/observer.h"
#include "context_internal.h"
#include "worktree_state.h"
#include "snapshot_verification.h"
#include "generated_io.h"
#include "shin/observer_protocol.h"
#include <chrono>

namespace shin::observer {
namespace core = shin::context;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

Json sample_worktree(const Config& config, const std::string& root_identity, bool source_only) {
    const auto start = std::chrono::steady_clock::now();
    Json result{{"status", "error"}, {"reason_code", nullptr}, {"started_at_ms", now_ms()},
        {"snapshot_id", nullptr}, {"worktree_id", nullptr}, {"snapshot_current", nullptr},
        {"profile_config_hash", nullptr}, {"source_counts", nullptr}, {"changes", nullptr}, {"details_omitted", 0},
        {"policy", "periodic-profile-content-comparison"}, {"verification_level", "local-source-observed"},
        {"source_mutated", false}, {"index_mutated", false}, {"git_required", false}, {"readiness_assessed", false}};
    std::string stage = "root-unavailable";
    std::string diagnostic_stage, operation;
    const auto detail = [&](const char* category) {
        return shin::observer_protocol::bounded_observation_diagnostic(Json{
            {"schema_version", "shin.source-observation-diagnostic.v1"},
            {"stage", diagnostic_stage}, {"operation", operation}, {"category", category}});
    };
    try {
        const auto verify_root = [&] {
            if (directory_identity(config.root) != root_identity)
                throw Error("root-identity-changed", "Source directory binding changed.");
        };
        verify_root();
        stage = "profile-unavailable";
        directory_identity(config.profile_dir);
        const auto profile_file = config.profile_dir / (config.profile + ".json");
        const auto profile_bytes = read_text(profile_file, 1024 * 1024, false);
        core::RuntimeOptions options{.root = config.root, .out_root = config.out_dir, .profile_dir = config.profile_dir,
            .profile = core::make_profile(config.profile, config.profile_dir), .force_index = true,
            .force_source_scan = false, .generated_snapshot = {}};
        if (options.profile.config_hash != core::fnv1a64(profile_bytes))
            throw Error("profile-changed", "Profile changed during observation.");
        result["profile_config_hash"] = options.profile.config_hash;

        stage = "snapshot-unavailable";
        std::error_code error;
        const auto output_status = fs::symlink_status(config.out_dir, error);
        if (error && error != std::errc::no_such_file_or_directory)
            throw Error(stage, "Cannot observe the local publication.");
        std::unique_ptr<shin::generated_io::IndexLock> lock;
        std::optional<core::VerifiedSnapshot> snapshot;
        std::string output_identity;
        if (fs::exists(output_status)) {
            output_identity = directory_identity(config.out_dir);
            const auto generated = fs::symlink_status(config.out_dir / core::kGeneratedDir, error);
            if (error && error != std::errc::no_such_file_or_directory)
                throw Error(stage, "Cannot observe the local publication.");
            if (!source_only && (fs::exists(generated) || shin::generated_io::publication_in_progress(config.out_dir))) {
                lock = core::lock_index_for_read(config.out_dir);
                snapshot = core::load_verified_snapshot(config.out_dir, config.max_snapshot_bytes, config.max_file_bytes);
            }
        }
        stage = "source-observation-failed";
        diagnostic_stage = "source-inventory"; operation = "observe-source";
        const auto inventory = core::observe_worktree(options);
        diagnostic_stage = "source-identity"; operation = "build-identity";
        result["worktree_id"] = core::fnv1a64(core::worktree_identity(options, inventory).dump());
        result["source_counts"] = {{"documents", inventory.documents.size()}, {"source_facts", inventory.source_facts.size()},
            {"candidate_files", inventory.source_fact_candidate_files}, {"capped_files", inventory.source_fact_capped_files},
            {"skipped_files", std::count_if(inventory.source_facts.begin(), inventory.source_facts.end(),
                [](const auto& file) { return file.second.status != "scanned"; })}};
        const bool complete = core::worktree_observation_complete(inventory);
        result["status"] = complete ? "unpublished" : "incomplete";
        result["reason_code"] = complete ? Json("index-unavailable") : Json("incomplete-source-observation");
        if (snapshot) {
            diagnostic_stage = "snapshot-comparison"; operation = "compare-snapshot";
            const auto comparison = core::compare_worktree(options, snapshot->manifest, inventory, 1);
            result["snapshot_id"] = snapshot->snapshot_id;
            const bool format_current = snapshot->generator_version == core::kGeneratorVersion;
            result["snapshot_current"] = complete && comparison.at("current").get<bool>() && format_current;
            result["changes"] = comparison.at("summary");
            // Paths and snippets stay in explicit Core retrieval, not persistent Observer telemetry.
            result["details_omitted"] = comparison.at("summary").at("total_changes");
            if (complete) {
                result["status"] = result["snapshot_current"] == true ? "current" : "stale";
                result["reason_code"] = result["snapshot_current"] == true ? Json(nullptr) :
                    Json(format_current ? "worktree-not-current" : "index-format-upgrade");
            }
        }
        diagnostic_stage = "source-stability"; operation = "verify-inventory";
        core::require_worktree_stable(options, inventory);
        stage = "profile-unavailable";
        diagnostic_stage = "profile-verification"; operation = "verify-profile";
        if (read_text(profile_file, 1024 * 1024, false) != profile_bytes)
            throw Error("profile-changed", "Profile changed during observation.");
        diagnostic_stage.clear(); operation.clear();
        stage = "snapshot-unavailable";
        if (!output_identity.empty() && directory_identity(config.out_dir) != output_identity)
            throw Error(stage, "Publication directory changed during observation.");
        stage = "root-unavailable";
        verify_root();
    } catch (const Error& error) {
        result["status"] = "error";
        result["reason_code"] = error.code == "root-identity-changed" || error.code == "profile-changed" ||
            error.code == "access-denied" || error.code == "storage-full" || error.code == "storage-read-only" ? error.code : stage;
        const auto diagnostic = detail(error.code == "access-denied" ? "access-denied" :
            error.code == "profile-changed" ? "entry-changed" : "io-error");
        if (!diagnostic.is_null()) result["diagnostic"] = diagnostic;
        result["snapshot_current"] = nullptr;
    } catch (const core::CliError& error) {
        result["status"] = "error";
        result["reason_code"] = error.code();
        const auto diagnostic = shin::observer_protocol::bounded_observation_diagnostic(error.diagnostic());
        if (!diagnostic.is_null()) result["diagnostic"] = diagnostic;
        result["snapshot_current"] = nullptr;
    } catch (const std::bad_alloc&) {
        result["status"] = "error";
        result["reason_code"] = stage;
        const auto diagnostic = detail("resource-exhausted");
        if (!diagnostic.is_null()) result["diagnostic"] = diagnostic;
        result["snapshot_current"] = nullptr;
    } catch (const std::exception&) {
        result["status"] = "error";
        result["reason_code"] = stage;
        const auto diagnostic = detail("unexpected-error");
        if (!diagnostic.is_null()) result["diagnostic"] = diagnostic;
        result["snapshot_current"] = nullptr;
    }
    if (result.at("status") == "error") {
        for (const auto* key : {"snapshot_id", "worktree_id", "snapshot_current", "profile_config_hash", "source_counts", "changes"})
            result[key] = nullptr;
        result["details_omitted"] = 0;
    }
    result["observed_at_ms"] = now_ms();
    result["duration_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}
} // namespace shin::observer
