#include "shin/observer/observer.h"
#if SHIN_OBSERVER_SOURCE_DISCOVERY
#include "context_internal.h"
#include "generated_io.h"
#include "generation_admission.h"
#include "snapshot_verification.h"
#include "source_discovery.h"
#include <chrono>
#endif

namespace shin::observer {
namespace {
enum class SourceOperation { query, context, inspect, read };

Json source_operation(const Config& config, SourceOperation operation, const std::string& input,
                      const std::string& expected_snapshot, const fs::path& private_cache,
                      std::shared_ptr<const shin::context::GenerationAdmission> admission) {
#if SHIN_OBSERVER_SOURCE_DISCOVERY
    namespace core = shin::context;
    const bool search = operation == SourceOperation::query || operation == SourceOperation::context;
    if (search && (input.empty() || input.size() > 4096 || input.find('\0') != std::string::npos))
        throw Error("invalid-source-query", "Source query must contain 1..4096 bytes without NUL.");
    if (operation == SourceOperation::inspect && (expected_snapshot.size() != 24 ||
        !expected_snapshot.starts_with("fnv1a64:") || !std::all_of(expected_snapshot.begin() + 8,
        expected_snapshot.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })))
        throw Error("invalid-source-snapshot", "Source inspection requires an exact snapshot identity.");
    const auto started = std::chrono::steady_clock::now();
    const auto status = observer_status(config);
    const auto& state = status.at("state");
    if (state.is_null()) throw Error("state-unavailable", "Sample this Observer binding before querying source.");
    const auto identity = state.at("root_identity").get<std::string>();
    if (directory_identity(config.root) != identity)
        throw Error("root-identity-changed", "Source directory binding changed.");
    const auto output_identity = directory_identity(config.out_dir);
    try {
        core::RuntimeOptions options{.root = config.root, .out_root = config.out_dir, .profile_dir = config.profile_dir,
            .profile = core::make_profile(config.profile, config.profile_dir), .force_index = true,
            .force_source_scan = false, .generated_snapshot = {}};
        options.generation_admission = std::move(admission);
        // Preserve Observer's snapshot limits and fence publication through the
        // request. Saved observation age/status is never currentness authority.
        const auto publication = core::lock_index_for_read(config.out_dir);
        const auto snapshot = core::load_verified_snapshot(config.out_dir, config.max_snapshot_bytes,
                                                          config.max_file_bytes).snapshot_id;
        if (options.generation_admission) options.generation_admission->assess(options);
        Json result;
        const char* key = nullptr;
        const char* schema = nullptr;
        if (search) {
            core::SourceDiscoveryIndex index(options, private_cache.empty() ? config.state_dir / "source-cache" : private_cache);
            const bool materialize = operation == SourceOperation::context;
            result = materialize ? index.context(options, input) : index.query(options, {input});
            key = materialize ? "source_context" : "source_candidates";
            schema = materialize ? "shin.observer-source-context.v1" : "shin.observer-source-query.v1";
        } else if (operation == SourceOperation::inspect) {
            if (expected_snapshot != snapshot)
                throw Error("source-snapshot-mismatch", "Inspection belongs to a different publication.");
            // Follow-up inherits strict Core admission; source-hash-only search
            // admission is not silently substituted for ordinary inspect/read.
            core::RetrievalSession session(options, core::RetrievalSessionOpenMode::verified_current);
            result = session.inspect(input);
            const auto freshness = core::assess_index_freshness(options, true);
            core::require_current_index_or_throw(freshness);
            if (freshness.snapshot_id != snapshot)
                throw Error("source-binding-changed", "Inspection publication changed before return.");
            key = "source_inspection";
            schema = "shin.observer-source-inspection.v1";
        } else {
            core::RetrievalSession session(options, core::RetrievalSessionOpenMode::verified_snapshot);
            result = session.read(input);
            key = "source_read";
            schema = "shin.observer-source-read.v1";
        }
        // A borrowed admission belongs to the enclosing local request, whose
        // final guard rehashes all bytes before releasing the publication lock.
        if (options.generation_admission) options.generation_admission->assess(options);
        const auto after = observer_status(config);
        if (after.at("state").is_null() || after.at("state").at("root_identity") != identity ||
            directory_identity(config.root) != identity || directory_identity(config.out_dir) != output_identity ||
            result.at("snapshot_id") != snapshot)
            throw Error("source-binding-changed", "Source query binding changed before return.");
        return {{"schema_version", schema}, {"binding_key", config.binding_key},
            {"project_id", config.binding.at("project_id")}, {"workspace_id", config.binding.at("workspace_id")},
            {"live_source_verified", true}, {"readiness_assessed", false},
            {"consumer_mutated", false}, {"index_mutated", false}, {"observer_state_mutated", false},
            {"delivery_performed", false}, {"query_persisted", false},
            {"duration_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()},
            {key, std::move(result)}};
    } catch (const core::CliError& error) {
        throw Error(error.code(), "Source query refused incomplete, stale or unsafe input.");
    }
#else
    (void)config;
    (void)operation;
    (void)input;
    (void)expected_snapshot;
    (void)private_cache;
    (void)admission;
    throw Error("source-discovery-unavailable", "This Observer build does not include source discovery.");
#endif
}
}

Json query_source(const Config& config, const std::string& query, bool materialize, const fs::path& private_cache,
                  std::shared_ptr<const shin::context::GenerationAdmission> admission) {
    return source_operation(config, materialize ? SourceOperation::context : SourceOperation::query, query, {}, private_cache, std::move(admission));
}
Json inspect_source(const Config& config, const std::string& path, const std::string& snapshot,
                    std::shared_ptr<const shin::context::GenerationAdmission> admission) {
    return source_operation(config, SourceOperation::inspect, path, snapshot, {}, std::move(admission));
}
Json read_source(const Config& config, const std::string& request,
                 std::shared_ptr<const shin::context::GenerationAdmission> admission) {
    return source_operation(config, SourceOperation::read, request, {}, {}, std::move(admission));
}
}
