#pragma once
#include "shin/observer/local_changes.h"
#include "generation_admission.h"
#include "generated_io.h"
#include "local_source_relevance.h"
#include "local_refresh_resources.h"
#include <utility>

namespace shin::observer {
// Ephemeral process-owned continuation of a full background qualification. It is
// never serialized, accepted from RPC, or used as request/source-read authority.
class LocalRefreshSession {
public:
    bool reuse(shin::context::RuntimeOptions& options, const fs::path& workspace, const std::string& key, Json& result) {
        namespace core = shin::context;
        const auto start = std::chrono::steady_clock::now();
        miss_reason_ = "no-checkpoint";
        const auto miss = [&](const char* reason) {
            miss_reason_ = reason;
            return false;
        };
        if (!changes_ || key_ != key || !changes_->healthy()) {
            if (changes_ && !changes_->healthy()) miss_reason_ = "observation-unavailable";
            else if (admission_ && key_ != key) miss_reason_ = "binding-changed";
            admission_.reset(); result_ = Json(); weight_ = 0; key_ = key;
            changes_.reset();
            // Reserve for opening a new monitor as well as the full Core pass.
            // Existing monitors do not need the extra allocation reserve.
            if (!local_refresh_descriptor_headroom(2 * kRefreshActiveDescriptorReserve)) return miss("descriptor-budget");
            core::RuntimeOptions watched;
            watched.root = options.root; watched.out_root = options.out_root;
            watched.profile = options.profile;
            changes_ = std::make_unique<LocalChanges>(std::vector<fs::path>{options.root, options.out_root, workspace},
                [watched = std::move(watched), workspace](const fs::path& path, bool directory) {
                    const auto generated = watched.out_root / core::kGeneratedDir;
                    if (local_path_below(path, generated) || (directory && local_path_below(generated, path))) return true;
                    const auto control = generated.parent_path();
                    // Core may rewrite its derived freshness cache during a full
                    // check. It is not admission input; publication controls are.
                    if (path == control / ".shin-context-publishing" || path == control / ".shin-context-owner" ||
                        local_path_below(path, control / ".shin-context-backup")) return true;
                    for (const auto* input : {"qualification", "config", "lifecycle", "publication", "profile-revisions", "config-policy", "profile-location"}) {
                        const auto base = workspace / input;
                        if (local_path_below(path, base) || (directory && local_path_below(base, path))) return true;
                    }
                    if (path == workspace / "state.json") return true;
                    return local_source_relevant(watched, path, directory);
                });
            // WatchRoot can pin every ancestor of several roots. A failed or
            // expensive monitor must release those handles before full Core IO.
            if (!changes_->healthy()) {
                changes_.reset();
                return miss("observation-unavailable");
            }
            if (!local_refresh_descriptor_headroom(kRefreshActiveDescriptorReserve)) {
                changes_.reset();
                return miss("descriptor-budget");
            }
        }
        changes_->flush();
        attempt_epoch_ = changes_->sequence();
        if (!admission_ || !changes_->healthy() || attempt_epoch_ != qualified_epoch_) {
            if (!changes_->healthy()) miss_reason_ = "observation-unavailable";
            else if (admission_) miss_reason_ = "filesystem-event";
            admission_.reset(); weight_ = 0; return false;
        }
        try {
            const auto lock = core::lock_index_for_read(options.out_root);
            (void)admission_->assess(options); // full membership, metadata, dependency and output-layout fence
            changes_->flush();
            if (!changes_->healthy() || changes_->sequence() != qualified_epoch_) {
                admission_.reset(); weight_ = 0;
                return miss(changes_->healthy() ? "filesystem-event" : "observation-unavailable");
            }
            options.generation_admission = admission_;
            result = result_;
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            auto& observation = result["observation"];
            observation["policy"] = "native-events-and-periodic-metadata-reconciliation";
            observation["verification_level"] = "source-inventory-and-metadata-observed";
            observation["content_reverified"] = false;
            observation["observed_at_ms"] = now_ms();
            observation["started_at_ms"] = observation.at("observed_at_ms").get<std::int64_t>() - static_cast<std::int64_t>(elapsed);
            observation["duration_ms"] = elapsed;
            result["rebuilt"] = false;
            result["prepared_new_snapshot"] = false;
            if (result.contains("recovered")) result["recovered"] = "";
            result["refresh_work"] = {{"idle_checkpoint_hit", true}, {"metadata_reconciliation_ms", elapsed},
                {"source_content_reverified", false}, {"generated_content_reverified", false}};
            if (result.dump(2).size() + 1 > 65536) {
                admission_.reset(); weight_ = 0; return miss("response-budget");
            }
            return true;
        } catch (const std::exception&) {
            // The ordinary full path owns recovery and the exact refusal. A
            // metadata mismatch is never permission to accept changed content.
            admission_.reset(); weight_ = 0; return miss("reconciliation-failed");
        }
    }

    void remember(const shin::context::RuntimeOptions& options, const Json& result) {
        if (!changes_ || !options.generation_admission || !result.value("ok", false) || result.value("phase", "") != "current" ||
            result.value("recovery_pending", false) || result.value("rebuilt", false) ||
            !result.contains("observation") || !result.at("observation").value("snapshot_current", false)) return;
        changes_->flush();
        if (!changes_->healthy() || changes_->sequence() != attempt_epoch_) return;
        const auto counts = result.at("observation").value("source_counts", Json::object());
        if (!counts.contains("source_facts") || !counts.contains("documents")) return;
        weight_ = counts.at("source_facts").get<std::size_t>() + counts.at("documents").get<std::size_t>() + 1;
        if (weight_ > 8192) { weight_ = 0; return; }
        admission_ = options.generation_admission;
        result_ = result;
        qualified_epoch_ = attempt_epoch_;
    }
    std::size_t weight() const { return weight_; }
    const char* take_miss_reason() { return std::exchange(miss_reason_, "not-eligible"); }
private:
    std::unique_ptr<LocalChanges> changes_;
    std::shared_ptr<const shin::context::GenerationAdmission> admission_;
    Json result_;
    std::string key_;
    std::uint64_t attempt_epoch_ = 0, qualified_epoch_ = 0;
    std::size_t weight_ = 0;
    const char* miss_reason_ = "not-eligible";
};
}
