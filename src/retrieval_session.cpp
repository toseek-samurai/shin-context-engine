#include "context_internal.h"
#include "generated_io.h"
#include "snapshot_verification.h"
#include "generated_snapshot.h"
#include <system_error>

namespace shin::context {
namespace {

void require_supported_generator(std::string_view generator_version) {
    if (generator_version == kGeneratorVersion) {
        return;
    }
    throw_index_error(
        "index-generator-unsupported",
        "Generated index compatibility identity does not match this Shin binary. Rebuild the index before retrieval.",
        false);
}

const RuntimeOptions& loaded_options(const std::shared_ptr<const GeneratedSnapshot>& snapshot) {
    if (!snapshot) throw_usage_error("invalid-snapshot-load", "A loaded snapshot is required.");
    return snapshot->options();
}

}  // namespace

std::unique_ptr<shin::generated_io::IndexLock> lock_index_for_read(const fs::path& out_root) {
    try {
        return std::make_unique<shin::generated_io::IndexLock>(
            out_root, shin::generated_io::LockMode::shared_existing);
    } catch (const std::system_error& error) {
        if (error.code() == std::errc::no_such_file_or_directory) {
            throw_index_error("index-unavailable", "An existing published index and reader lock are required.");
        }
        throw;
    }
}

RetrievalSession::RetrievalSession(RuntimeOptions options, bool require_current)
    : RetrievalSession(std::move(options),
                       require_current
                           ? RetrievalSessionOpenMode::require_current
                           : RetrievalSessionOpenMode::fast_freshness) {}

RetrievalSession::RetrievalSession(RuntimeOptions options,
                                   RetrievalSessionOpenMode mode)
    : options_(std::move(options)),
      lock_(lock_index_for_read(options_.out_root)) {
    if (options_.generated_snapshot)
        throw_usage_error("invalid-snapshot-session", "Construct a loaded session from its snapshot owner.");
    if (!generated_index_available(options_)) {
        throw_index_error("index-unavailable", "A complete generated index is required.");
    }
    const bool verify_snapshot = mode == RetrievalSessionOpenMode::verified_snapshot ||
                                 mode == RetrievalSessionOpenMode::verified_current;
    const bool require_current = mode == RetrievalSessionOpenMode::require_current ||
                                 mode == RetrievalSessionOpenMode::verified_current;
    if (verify_snapshot) {
        VerifiedSnapshot snapshot = load_verified_snapshot(options_.out_root);
        require_supported_generator(snapshot.generator_version);
        freshness_.status = "not-assessed";
        freshness_.mode = "verified-generated-snapshot";
        freshness_.snapshot_id = snapshot.snapshot_id;
        freshness_.source_count = static_cast<int>(snapshot.sources.size());
        freshness_.notes.push_back(
            "Generated snapshot integrity is verified; source freshness is not assessed.");
        manifest_ = std::move(snapshot.manifest);
        if (!require_current) {
            return;
        }
    }

    freshness_ = assess_index_freshness(options_, require_current);
    if (require_current) {
        require_current_index_or_throw(freshness_);
    }
    const fs::path manifest_path = options_.out_root / kGeneratedDir / "manifest.json";
    const std::string manifest_text = read_file(manifest_path);
    if (freshness_.snapshot_id.empty() || fnv1a64(manifest_text) != freshness_.snapshot_id) {
        throw CliError("snapshot-changed-during-session-open", "index",
                       "The generated snapshot changed while the retrieval session opened.", 1,
                       true, "Retry after the concurrent publisher has completed.");
    }
    manifest_ = parse_json_text(manifest_text, manifest_path.string());
    if (!manifest_.is_object() ||
        json_string_value(manifest_, "schema_version") != "shin.context.kb-enterprise.v1" ||
        json_string_value(manifest_, "tool") != "shin-context") {
        throw_index_error("index-corrupt",
                          "Generated manifest has an unsupported schema or tool.", false);
    }
    require_supported_generator(json_string_value(manifest_, "generator_version"));
}

RetrievalSession::~RetrievalSession() = default;

const Json& RetrievalSession::manifest() const noexcept {
    return options_.generated_snapshot ? options_.generated_snapshot->manifest() : manifest_;
}

RetrievalSession::RetrievalSession(std::shared_ptr<const GeneratedSnapshot> snapshot, bool require_current)
    : RetrievalSession(std::move(snapshot), require_current ? RetrievalSessionOpenMode::require_current :
                                                            RetrievalSessionOpenMode::fast_freshness) {}

RetrievalSession::RetrievalSession(std::shared_ptr<const GeneratedSnapshot> snapshot, RetrievalSessionOpenMode mode)
    : options_(loaded_options(snapshot)) {
    options_.generated_snapshot = std::move(snapshot);
    const bool pinned = mode == RetrievalSessionOpenMode::verified_snapshot ||
                        mode == RetrievalSessionOpenMode::verified_current;
    if (pinned) lock_ = lock_index_for_read(options_.out_root);
    freshness_.status = "not-assessed";
    freshness_.mode = "verified-generated-snapshot";
    freshness_.snapshot_id = options_.generated_snapshot->id();
    freshness_.source_count = static_cast<int>(manifest().at("sources").size());
    freshness_.notes.push_back("Generated snapshot integrity is verified; source freshness is not assessed.");
    if (mode == RetrievalSessionOpenMode::require_current || mode == RetrievalSessionOpenMode::verified_current) {
        // Admission proof is short-lived; resident bytes never retain the publication lock.
        auto lock = pinned ? nullptr : lock_index_for_read(options_.out_root);
        freshness_ = assess_index_freshness(options_, true);
        require_current_index_or_throw(freshness_);
        if (freshness_.snapshot_id != options_.generated_snapshot->id())
            throw_index_error("snapshot-generation-not-current", "Loaded generation is no longer the current publication.");
    }
}

}  // namespace shin::context
