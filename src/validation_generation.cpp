#include "validation_generation.h"
#include "context_internal.h"
#include "generated_row_decoder.h"

namespace shin::context {
namespace {
void changed() {
    throw_index_error("validation-inputs-changed", "Validation generation inputs changed.");
}
}

ValidationGeneration::ValidationGeneration(const RuntimeOptions& options, std::uintmax_t maximum_bytes)
    : root_(options.root), out_root_(options.out_root), profile_dir_(options.profile_dir),
      profile_hash_(profile_config_hash(options.profile)), profile_(options.profile),
      maximum_bytes_(std::min(maximum_bytes, std::uintmax_t{256ULL * 1024 * 1024})) {}

void ValidationGeneration::verify_binding(const RuntimeOptions& options) const {
    if (root_ != options.root || out_root_ != options.out_root || profile_dir_ != options.profile_dir ||
        profile_ != options.profile || profile_hash_ != profile_config_hash(options.profile) ||
        profile_hash_ != profile_config_hash(make_profile(options.profile.id, options.profile_dir))) changed();
}

void ValidationGeneration::verify_inputs(const RuntimeOptions& options) {
    const auto started = SteadyClock::now();
    const auto fence = [&] {
        verify_binding(options);
        ++inventory_checks_;
        require_worktree_stable(options, sources_);
        for (const auto& [path, exists] : links_)
            if (fs::exists(root_ / path) != exists) changed();
        if (retool_ && retool_generation_environment(root_, fact_paths_) != retool_environment_) changed();
    };
    fence();
    std::map<std::string, FileFingerprint> files;
    for (const auto* states : {&sources_.documents, &sources_.source_facts})
        for (const auto& [path, state] : *states) {
            const FileFingerprint expected{state.metadata, state.content_hash};
            const auto [entry, inserted] = files.emplace(path, expected);
            if (!inserted && (entry->second.metadata != expected.metadata ||
                              entry->second.content_hash != expected.content_hash)) changed();
        }
    std::vector<std::pair<std::string, FileFingerprint>> selected(files.begin(), files.end());
    parallel_for_indices(selected.size(), [&](std::size_t index) {
        const auto& [path, expected] = selected[index];
        const auto current = fingerprint_file(root_ / path);
        if (current.metadata != expected.metadata || current.content_hash != expected.content_hash) changed();
    });
    hash_files_ += selected.size();
    for (const auto& [path, expected] : selected) { (void)path; hash_bytes_ += expected.metadata.byte_count; }
    fence();
    verification_ms_ += elapsed_ms_since(started);
}

std::shared_ptr<const GeneratedOutput> ValidationGeneration::acquire(const RuntimeOptions& options, bool compact) {
    const std::lock_guard guard(mutex_);
    if (disabled_) return {};
    try {
        verify_binding(options);
        if (generated_) {
            if (compact != compact_) changed();
            verify_inputs(options);
            ++reuses_;
            return generated_;
        }
        for (const auto& extractor : options.profile.source_fact_extractors) {
            if (extractor != "language" && extractor != "retool") {
                disabled_ = true; fallback_reason_ = "unsupported-dependencies"; return {};
            }
            retool_ = retool_ || extractor == "retool";
        }
        sources_ = observe_worktree(options);
        if (!worktree_observation_complete(sources_)) {
            disabled_ = true; fallback_reason_ = "incomplete-source-observation"; return {};
        }
        for (const auto& [path, state] : sources_.source_facts) { (void)state; fact_paths_.push_back(path); }
        if (retool_) retool_environment_ = retool_generation_environment(root_, fact_paths_);
        compact_ = compact;
        auto generated = std::make_shared<const GeneratedOutput>(generate_outputs(options.profile, root_, nullptr, compact));
        // Dependencies come from the newly computed expectation, never from a
        // possibly stale/forged index. A previously broken target matters too.
        const auto found = generated->files.find((kGeneratedDir / "links.jsonl").generic_string());
        if (found != generated->files.end()) {
            std::istringstream stream(found->second);
            for (std::string line; std::getline(stream, line);) {
                const auto link = decode_generated_link_row(line);
                if ((link.kind == "markdown-link" || link.kind == "path-reference") &&
                    (link.status == "ok" || link.status == "broken")) {
                    const auto [entry, inserted] = links_.emplace(link.resolved_path, link.status == "ok");
                    if (!inserted && entry->second != (link.status == "ok")) changed();
                }
            }
        }
        verify_inputs(options);
        std::uintmax_t bytes = 0;
        for (const auto& [path, content] : generated->files) { (void)path; bytes += content.size(); }
        if (bytes > maximum_bytes_) {
            disabled_ = true; fallback_reason_ = "retained-byte-limit";
            return generated; // usable by this gate, never retained for later ones
        }
        generated_ = std::move(generated);
        return generated_;
    } catch (const std::bad_alloc&) { throw; }
    catch (const CliError& error) { fallback_reason_ = error.code(); }
    catch (const std::exception&) { fallback_reason_ = "observation-unavailable"; }
    // A failed observation is not a failed gate. Discard reuse permanently for
    // this invocation and let the normal independent commands produce diagnostics.
    ++invalidations_;
    disabled_ = true;
    generated_.reset();
    return {};
}

Json ValidationGeneration::metrics() const {
    const std::lock_guard guard(mutex_);
    return {{"policy", "invocation-local-expected-generation-v1"}, {"reuses", reuses_},
        {"invalidations", invalidations_}, {"inventory_checks", inventory_checks_},
        {"verified_source_files", hash_files_}, {"verified_source_bytes", hash_bytes_},
        {"verification_ms", verification_ms_}, {"retained_bytes", generated_ ? generated_->generated_bytes : 0},
        {"fallback_reason", fallback_reason_}};
}

std::shared_ptr<const GeneratedOutput> validation_generated_output(const RuntimeOptions& options, bool compact) {
    if (options.validation_generation)
        if (auto generated = options.validation_generation->acquire(options, compact)) return generated;
    return std::make_shared<const GeneratedOutput>(generate_outputs(options.profile, options.root, nullptr, compact));
}
} // namespace shin::context
