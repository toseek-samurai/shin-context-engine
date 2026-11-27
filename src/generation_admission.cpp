#include "project_files.h"
#include "generation_admission.h"
#include "context_internal.h"
#include "generated_row_decoder.h"
#include "snapshot_verification.h"

namespace shin::context {
namespace {
std::vector<std::string> fact_paths(const WorktreeInventory& sources) {
    std::vector<std::string> paths;
    for (const auto& [path, state] : sources.source_facts) {
        (void)state;
        paths.push_back(path);
    }
    return paths;
}
void changed() {
    throw_index_error("generation-admission-changed", "Qualified generation inputs or output changed during this request.");
}

void verify_generation_binding(const RuntimeOptions& options, const fs::path& root, const fs::path& out_root,
                               const std::string& profile_hash) {
    if (root != options.root || out_root != options.out_root ||
        profile_hash != profile_config_hash(options.profile) ||
        profile_hash != profile_config_hash(make_profile(options.profile.id, options.profile_dir))) changed();
}

template <typename Links>
void verify_links(const RuntimeOptions& options, const Links& links) {
    for (const auto& [path, exists] : links)
        if (observed_link_exists(options.profile.automatic_project_files, options.root, path) != exists) changed();
}

std::size_t verify_files(const std::map<fs::path, FileFingerprint>& files) {
    std::vector<const std::map<fs::path, FileFingerprint>::value_type*> selected;
    selected.reserve(files.size());
    for (const auto& entry : files) selected.push_back(&entry);
    parallel_for_indices(selected.size(), [&](std::size_t index) {
        const auto& [path, expected] = *selected[index];
        const auto current = fingerprint_file(path);
        if (current.metadata != expected.metadata || current.content_hash != expected.content_hash) changed();
    });
    return selected.size();
}
} // namespace

GenerationAdmission::GenerationAdmission(const RuntimeOptions& options, const VerifiedSnapshot* observed_snapshot,
                                          bool audit_automatic_sources)
    : root_(options.root), out_root_(options.out_root),
      profile_hash_(profile_config_hash(options.profile)),
      source_audit_(audit_automatic_sources && options.profile.automatic_project_files
          ? std::optional(DoctorSourceAudit{}) : std::nullopt),
      sources_(observe_worktree(options, source_audit_ ? &*source_audit_ : nullptr)) {
    const auto paths = expected_generated_relative_files(options);
    verify_snapshot_filesystem(out_root_, {paths.begin(), paths.end()});
    if (observed_snapshot && (observed_snapshot->out_root != out_root_ ||
        observed_snapshot->fingerprints.size() != paths.size())) changed();
    Json output_identity = Json::array();
    for (const auto& path : paths) {
        FileFingerprint value;
        if (observed_snapshot) {
            const auto found = observed_snapshot->fingerprints.find(path);
            if (found == observed_snapshot->fingerprints.end() ||
                file_metadata(out_root_ / path) != found->second.metadata) changed();
            value = found->second;
            reused_output_bytes_ += value.metadata.byte_count;
            ++reused_output_files_;
        } else value = fingerprint_file(out_root_ / path);
        output_identity.push_back({{"path", path}, {"bytes", value.metadata.byte_count}, {"hash", value.content_hash}});
        if (path == (kGeneratedDir / "manifest.json").generic_string()) snapshot_ = value.content_hash;
        outputs_.emplace(path, std::move(value));
    }
    // The exact audit certified the dependency list together with all its input
    // and output bytes. Re-probe its external existence dependencies, not the
    // document parser. An uncertified/altered list can never match the receipt.
    std::istringstream links(read_file(out_root_ / kGeneratedDir / "links.jsonl"));
    for (std::string line; std::getline(links, line);) {
        const auto link = decode_generated_link_row(line);
        if (link.kind == "markdown-link" || link.kind == "path-reference") {
            if (link.status == "ok" || link.status == "broken")
                links_[link.resolved_path] = observed_link_exists(options.profile.automatic_project_files, root_, link.resolved_path);
        }
    }
    for (const auto& extractor : options.profile.source_fact_extractors) {
        if (extractor != "language" && extractor != "retool")
            throw_index_error("generation-admission-unsupported", "Extractor dependency closure is not known.");
        // Automatic Retool layout is derived solely from the admitted inventory.
        if (extractor == "retool" && !options.profile.automatic_project_files) retool_ = true;
    }
    if (retool_) retool_environment_ = retool_generation_environment(root_, fact_paths(sources_));
    Json identity{{"policy", "generation-inputs-and-output-v1"},
        {"root", root_.generic_string()}, {"out_root", out_root_.generic_string()},
        {"generator", kGeneratorVersion}, {"inputs", worktree_identity(options, sources_)},
        {"outputs", output_identity}, {"links", links_}, {"retool", retool_environment_}};
    fingerprint_ = fnv1a64(identity.dump());
    identity.erase("out_root");
    portable_fingerprint_ = fnv1a64(identity.dump());
    verify_unchanged(options);
}

std::string generation_fingerprint(const RuntimeOptions& options) {
    return GenerationAdmission(options).fingerprint();
}

std::shared_ptr<const GenerationAdmission> GenerationAdmission::reuse(
    const RuntimeOptions& options, const std::string& qualified_fingerprint, const VerifiedSnapshot* observed_snapshot,
    std::function<bool()> unchanged_source_membership, bool audit_automatic_sources) {
    auto observed = std::shared_ptr<GenerationAdmission>(new GenerationAdmission(options, observed_snapshot, audit_automatic_sources));
    if (observed->fingerprint_ != qualified_fingerprint || !worktree_observation_complete(observed->sources_))
        return {};
    if (const auto& audit = observed->source_audit_; audit &&
        audit->scanned_files + static_cast<std::size_t>(audit->excluded_files) != observed->sources_.source_facts.size())
        return {};
    observed->unchanged_source_membership_ = std::move(unchanged_source_membership);
    return observed;
}

const DoctorSourceAudit* GenerationAdmission::doctor_source_audit(const RuntimeOptions& options) const {
    if (!source_audit_) return nullptr;
    verify_binding(options);
    return &*source_audit_;
}

std::shared_ptr<const GenerationAdmission> GenerationAdmission::published_copy(
    const RuntimeOptions& source, const RuntimeOptions& destination) const {
    verify_content(source);
    auto published = std::shared_ptr<const GenerationAdmission>(new GenerationAdmission(destination));
    if (!worktree_observation_complete(sources_) ||
        !worktree_observation_complete(published->sources_) ||
        published->portable_fingerprint_ != portable_fingerprint_) changed();
    // Both copies share identical source observations. Fence both output trees
    // and the complete common dependency closure while hashing shared paths once.
    verify_content_together(source, *published, destination);
    return published;
}

void GenerationAdmission::verify_binding(const RuntimeOptions& options) const {
    verify_generation_binding(options, root_, out_root_, profile_hash_);
}

void GenerationAdmission::verify_sources(const RuntimeOptions& options, bool complete_inventory) const {
    if (!complete_inventory && unchanged_source_membership_ && unchanged_source_membership_())
        require_worktree_metadata_stable(options, sources_);
    else require_worktree_stable(options, sources_);
    verify_links(options, links_);
    if (retool_ && retool_generation_environment(root_, fact_paths(sources_)) != retool_environment_) changed();
}

void GenerationAdmission::verify_outputs(const RuntimeOptions& options) const {
    const auto paths = expected_generated_relative_files(options);
    verify_snapshot_filesystem(out_root_, {paths.begin(), paths.end()});
    for (const auto& [path, value] : outputs_)
        if (file_metadata(out_root_ / path) != value.metadata) changed();
}

void GenerationAdmission::verify_unchanged(const RuntimeOptions& options, bool complete_inventory) const {
    verify_binding(options);
    verify_sources(options, complete_inventory);
    verify_outputs(options);
}

IndexFreshness GenerationAdmission::assess(const RuntimeOptions& options) const {
    verify_unchanged(options);
    IndexFreshness result;
    result.status = "current";
    result.mode = "qualified-generation-live-inputs";
    result.current = result.fully_verified = true;
    result.snapshot_id = snapshot_;
    result.source_count = static_cast<int>(sources_.documents.size());
    return result;
}

std::unique_ptr<const IdleGenerationCheckpoint> GenerationAdmission::idle_checkpoint(
    IdleGenerationCheckpoint::Digest digest) const {
    const auto metadata = worktree_metadata_payload(sources_);
    if (metadata.empty()) return {};
    auto result = std::unique_ptr<IdleGenerationCheckpoint>(new IdleGenerationCheckpoint(digest));
    result->root_ = root_;
    result->out_root_ = out_root_;
    result->profile_hash_ = profile_hash_;
    result->source_metadata_ = digest(metadata);
    if (retool_) result->retool_environment_ = digest(retool_environment_.dump());
    result->outputs_.reserve(outputs_.size());
    for (const auto& [path, value] : outputs_) result->outputs_.emplace_back(path, value.metadata);
    result->links_.assign(links_.begin(), links_.end());
    return result;
}

void IdleGenerationCheckpoint::verify(const RuntimeOptions& options) const {
    verify_generation_binding(options, root_, out_root_, profile_hash_);
    const auto live = observe_worktree_metadata(options);
    if (live.payload.empty() || digest_(live.payload) != source_metadata_) changed();
    verify_links(options, links_);
    if (!retool_environment_.empty() &&
        digest_(retool_generation_environment(root_, live.fact_paths).dump()) != retool_environment_) changed();
    const auto paths = expected_generated_relative_files(options);
    verify_snapshot_filesystem(out_root_, {paths.begin(), paths.end()});
    for (const auto& [path, metadata] : outputs_)
        if (file_metadata(out_root_ / path) != metadata) changed();
}

std::size_t IdleGenerationCheckpoint::retained_bytes() const noexcept {
    const auto storage = [](const auto& entries) {
        std::size_t bytes = entries.capacity() * sizeof(typename std::decay_t<decltype(entries)>::value_type);
        for (const auto& entry : entries) bytes += entry.first.capacity() + 1;
        return bytes;
    };
    return sizeof(*this) + (root_.native().capacity() + out_root_.native().capacity() + 2) * sizeof(fs::path::value_type) +
        profile_hash_.capacity() + source_metadata_.capacity() + retool_environment_.capacity() + 3 +
        storage(outputs_) + storage(links_);
}

void GenerationAdmission::append_content_files(std::map<fs::path, FileFingerprint>& files) const {
    for (const auto* sources : {&sources_.documents, &sources_.source_facts})
        for (const auto& [path, state] : *sources) {
            const FileFingerprint expected{state.metadata, state.content_hash};
            const auto [entry, inserted] = files.emplace(root_ / path, expected);
            if (!inserted && (entry->second.metadata != expected.metadata ||
                              entry->second.content_hash != expected.content_hash)) changed();
        }
    for (const auto& [path, expected] : outputs_) {
        const auto [entry, inserted] = files.emplace(out_root_ / path, expected);
        if (!inserted && (entry->second.metadata != expected.metadata ||
                          entry->second.content_hash != expected.content_hash)) changed();
    }
}

void GenerationAdmission::verify_content(const RuntimeOptions& options) const {
    verify_unchanged(options, true);
    std::map<fs::path, FileFingerprint> files;
    append_content_files(files);
    verify_files(files);
    verify_unchanged(options, true);
}

std::size_t GenerationAdmission::verify_content_together(const RuntimeOptions& options,
    const GenerationAdmission& other, const RuntimeOptions& other_options) const {
    // Sharing a live inventory walk requires equal observations, including
    // metadata and external dependencies, not just the portable content digest.
    if (root_ != other.root_ || profile_hash_ != other.profile_hash_ ||
        portable_fingerprint_ != other.portable_fingerprint_ ||
        sources_.documents != other.sources_.documents || sources_.source_facts != other.sources_.source_facts ||
        sources_.source_fact_candidate_files != other.sources_.source_fact_candidate_files ||
        sources_.source_fact_capped_files != other.sources_.source_fact_capped_files ||
        links_ != other.links_ || retool_ != other.retool_ || retool_environment_ != other.retool_environment_ ||
        !worktree_observation_complete(sources_) || !worktree_observation_complete(other.sources_)) changed();
    const auto verify_both = [&] {
        verify_binding(options);
        other.verify_binding(other_options);
        verify_sources(options, true);
        verify_outputs(options);
        other.verify_outputs(other_options);
    };
    verify_both();
    std::map<fs::path, FileFingerprint> files;
    append_content_files(files);
    other.append_content_files(files);
    const auto verified_files = verify_files(files);
    verify_both();
    return verified_files;
}

} // namespace shin::context
