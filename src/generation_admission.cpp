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
std::size_t verify_files(const std::map<fs::path, FileFingerprint>& files) {
    std::vector<std::pair<fs::path, FileFingerprint>> selected(files.begin(), files.end());
    parallel_for_indices(selected.size(), [&](std::size_t index) {
        const auto& [path, expected] = selected[index];
        const auto current = fingerprint_file(path);
        if (current.metadata != expected.metadata || current.content_hash != expected.content_hash) changed();
    });
    return selected.size();
}
} // namespace

GenerationAdmission::GenerationAdmission(const RuntimeOptions& options, const VerifiedSnapshot* observed_snapshot)
    : root_(options.root), out_root_(options.out_root),
      profile_hash_(profile_config_hash(options.profile)), sources_(observe_worktree(options)) {
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
                links_[link.resolved_path] = fs::exists(root_ / link.resolved_path);
        }
    }
    for (const auto& extractor : options.profile.source_fact_extractors) {
        if (extractor != "language" && extractor != "retool")
            throw_index_error("generation-admission-unsupported", "Extractor dependency closure is not known.");
        retool_ = retool_ || extractor == "retool";
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
    const RuntimeOptions& options, const std::string& qualified_fingerprint, const VerifiedSnapshot* observed_snapshot) {
    auto observed = std::shared_ptr<const GenerationAdmission>(new GenerationAdmission(options, observed_snapshot));
    if (observed->fingerprint_ != qualified_fingerprint || !worktree_observation_complete(observed->sources_))
        return {};
    return observed;
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
    if (root_ != options.root || out_root_ != options.out_root ||
        profile_hash_ != profile_config_hash(options.profile) ||
        profile_hash_ != profile_config_hash(make_profile(options.profile.id, options.profile_dir))) changed();
}

void GenerationAdmission::verify_sources(const RuntimeOptions& options) const {
    require_worktree_stable(options, sources_);
    for (const auto& [path, exists] : links_)
        if (fs::exists(root_ / path) != exists) changed();
    if (retool_ && retool_generation_environment(root_, fact_paths(sources_)) != retool_environment_) changed();
}

void GenerationAdmission::verify_outputs(const RuntimeOptions& options) const {
    const auto paths = expected_generated_relative_files(options);
    verify_snapshot_filesystem(out_root_, {paths.begin(), paths.end()});
    for (const auto& [path, value] : outputs_)
        if (file_metadata(out_root_ / path) != value.metadata) changed();
}

void GenerationAdmission::verify_unchanged(const RuntimeOptions& options) const {
    verify_binding(options);
    verify_sources(options);
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
    verify_unchanged(options);
    std::map<fs::path, FileFingerprint> files;
    append_content_files(files);
    verify_files(files);
    verify_unchanged(options);
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
        verify_sources(options);
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
