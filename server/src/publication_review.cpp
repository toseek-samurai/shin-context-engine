#include "shin/server/publication_service.h"
#include "shin/server/candidate_artifact.h"
#include "shin/server/route_files.h"
#include "context_internal.h"
#include "snapshot_verification.h"
#include "generated_io.h"

#include <map>
#include <memory>
#include <vector>

namespace shin::server {
namespace {
namespace core = shin::context;

Json differences(const core::VerifiedSnapshot& from, const core::VerifiedSnapshot& to, std::size_t& remaining) {
    Json changes = Json::array();
    Json counts{{"added", 0}, {"removed", 0}, {"changed", 0}};
    std::size_t omitted = 0;
    const auto compare = [&](const auto& left, const auto& right, const char* kind) {
        const auto append = [&](const std::string& path, const char* change) {
            counts[change] = counts[change].get<std::size_t>() + 1;
            Json entry{{"path", path}, {"kind", kind}, {"change", change}};
            const auto bytes = entry.dump().size();
            if (changes.size() >= 32 || bytes > remaining) { ++omitted; return; }
            remaining -= bytes;
            changes.push_back(std::move(entry));
        };
        for (const auto& [path, hash] : left) {
            const auto match = right.find(path);
            if (match == right.end()) append(path, "removed");
            else if (hash != match->second) append(path, "changed");
        }
        for (const auto& [path, hash] : right) {
            (void)hash;
            if (!left.contains(path)) append(path, "added");
        }
    };
    compare(from.sources, to.sources, "document-source");
    compare(from.source_facts, to.source_facts, "fact-source");
    return {{"counts", counts}, {"changes", changes}, {"omitted_changes", omitted},
            {"same_profile", from.profile == to.profile && from.profile_config_hash == to.profile_config_hash},
            {"same_generator", from.generator_version == to.generator_version}};
}
} // namespace

Json PublicationService::review(const Principal& principal, const std::string& project,
                                 const std::string& publication, std::uint64_t id) {
    auto review_input = store_.publication_review_input(principal, project, publication, id);
    std::unique_lock admission(review_mutex_, std::try_to_lock);
    if (!admission.owns_lock()) throw ServiceError(503, "publication-review-busy", "Publication history is being reviewed or retired.");
    // Authorize before admission, then close the intervening retirement window.
    review_input = store_.publication_review_input(principal, project, publication, id);
    auto detail = review_input.at("detail");
    Json artifacts = Json::object();
    std::map<std::uint64_t, core::VerifiedSnapshot> snapshots;
    std::vector<std::unique_ptr<CandidateArtifactLock>> lifetimes;
    std::vector<std::unique_ptr<shin::generated_io::IndexLock>> publications;
    for (const auto* side : {"base", "current", "candidate"}) {
        const auto& candidate = detail.at(side).at("preparation_id");
        if (candidate.is_null()) { artifacts[side] = {{"state", "empty"}}; continue; }
        const auto preparation_id = candidate.get<std::uint64_t>();
        try {
            const auto input = store_.preparation_inspection_input(principal, preparation_id);
            const auto& job = input.at("job");
            const auto& origin = detail.at(side);
            if (!input.at("retention").is_null())
                throw ServiceError(409, "candidate-retention-pending", "Candidate is retained or released.");
            if (job.at("project_id") != project || job.at("state") != "prepared")
                throw ServiceError(409, "candidate-not-prepared", "Candidate is not available for review.");
            if (input.at("route_fingerprint") != review_input.at("fingerprints").at(side) ||
                job.at("route_id") != origin.at("source_route_id") ||
                job.at("artifact_snapshot_id") != origin.at("snapshot_id") ||
                input.at("artifact_namespace") != candidate_namespace(config_.preparations.state_dir, state_identity_))
                throw ServiceError(409, "candidate-ledger-mismatch", "Saved artifact provenance does not match its ledger.");
            if (!snapshots.contains(preparation_id)) {
                const auto key = input.at("artifact_key").get<std::string>();
                auto lifetime = std::make_unique<CandidateArtifactLock>(config_.preparations.state_dir, state_identity_, preparation_id, key,
                                                                        CandidateArtifactLock::Mode::inspect_existing);
                const auto root = config_.preparations.state_dir / candidate_artifact_name(preparation_id, key);
                const auto identity = verify_candidate_layout(root, config_.retrieval.max_file_bytes);
                auto lock = core::lock_index_for_read(root);
                auto snapshot = core::load_verified_snapshot(root, config_.retrieval.max_generation_bytes,
                                                             config_.retrieval.max_file_bytes);
                std::uintmax_t bytes = snapshot.manifest_content.size();
                for (const auto& [path, file] : snapshot.files) { (void)path; bytes += file.bytes; }
                if (snapshot.snapshot_id != job.at("artifact_snapshot_id").get<std::string>() ||
                    bytes != job.at("artifact_bytes").get<std::uintmax_t>() ||
                    snapshot.profile != origin.at("profile_id").get<std::string>() ||
                    verify_candidate_layout(root, config_.retrieval.max_file_bytes) != identity)
                    throw ServiceError(409, "candidate-artifact-invalid", "Candidate changed during review.");
                lifetime->verify();
                snapshots.emplace(preparation_id, std::move(snapshot));
                lifetimes.push_back(std::move(lifetime));
                publications.push_back(std::move(lock));
            }
            artifacts[side] = {{"state", "artifact-verified"}, {"snapshot_id", snapshots.at(preparation_id).snapshot_id}};
        } catch (const ServiceError& error) {
            artifacts[side] = {{"state", "unavailable"}, {"reason_code", error.code}};
        } catch (const core::CliError& error) {
            artifacts[side] = {{"state", "unavailable"}, {"reason_code", error.code()}};
        }
    }
    std::size_t remaining = 32768;
    Json comparisons = Json::object();
    const auto target = detail.at("candidate").at("preparation_id").get<std::uint64_t>();
    for (const auto* side : {"base", "current"}) {
        const auto& from_id = detail.at(side).at("preparation_id");
        if (artifacts.at(side).at("state") == "artifact-verified" && artifacts.at("candidate").at("state") == "artifact-verified" &&
            !from_id.is_null() && snapshots.contains(from_id.get<std::uint64_t>()) && snapshots.contains(target))
            comparisons[side] = differences(snapshots.at(from_id.get<std::uint64_t>()), snapshots.at(target), remaining);
        else comparisons[side] = {{"state", "not-compared"}};
    }
    for (const auto& lifetime : lifetimes) lifetime->verify();
    if (store_.publication_review_input(principal, project, publication, id) != review_input)
        throw ServiceError(409, "publication-review-changed", "Publication changed during review; request a new review.");
    detail["review"] = {{"schema_version", "shin.publication-review.v1"}, {"artifacts", artifacts},
        {"comparisons", comparisons}, {"scope", "artifact-source-fingerprints"}, {"source_currentness", "not-assessed"},
        {"publication_authorized", false}, {"semantic_equivalence", "not-assessed"}};
    if (detail.dump().size() > 65536)
        throw ServiceError(413, "publication-response-too-large", "Publication review exceeds its response bound.");
    return detail;
}
} // namespace shin::server
