#include "shin/observer/local_project.h"
#include "shin/observer/local_publication.h"
#include "shin/observer/local_query.h"
#include "shin/observer/local_changes.h"
#include "shin/observer/local_registry.h"
#include "shin/observer/local_identity.h"
#include "context_internal.h"
#include "profile_policy.h"
#include "generated_io.h"
#include "generation_admission.h"
#include "source_discovery.h"
#include "snapshot_verification.h"
#include "worktree_state.h"
#include "local_refresh_guard.h"
#include "local_profile_revision.h"
#include "local_partial_context.h"
#include "local_route_guard.h"
#include <chrono>
#include <cstdlib>
#include <thread>

namespace shin::observer {
namespace {
namespace core = shin::context;

bool overlaps(const fs::path& a, const fs::path& b) {
    const auto [left, right] = std::mismatch(a.begin(), a.end(), b.begin(), b.end());
    return left == a.end() || right == b.end();
}

void guard_unbound_workspace(const fs::path& workspace, const std::optional<Json>& binding) {
    if (binding) return;
    if (present(workspace / "state.pending"))
        throw Error("binding-uncommitted", "Uncommitted initial binding requires review; it will not be replaced.");
    for (const auto& entry : fs::directory_iterator(workspace))
        if (entry.path().filename() != "observer.lock")
            throw Error("unbound-project-state", "Private project data exists without its binding; preserve it for review, not init or attach.");
}

template<class Invoke> Json core_result(Invoke&& invoke) {
    // Core's CLI adapter is already used by ready; keep stream capture scoped
    // to this one-shot, single-threaded frontend, never a Server handler.
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());
    int code = -1;
    try { code = invoke(); }
    catch (...) { std::cout.rdbuf(previous); throw; }
    std::cout.rdbuf(previous);
    auto result = core::parse_json_text(output.str(), "native command result");
    if (!result.is_object() || !result.contains("ok") || !result.at("ok").is_boolean() ||
        result.at("ok").get<bool>() != (code == 0))
        throw Error("native-result-invalid", "Native command result is inconsistent.");
    return result;
}

Json bootstrap_readiness(const core::RuntimeOptions& options) {
    Json result = {{"ok", false}, {"scope", "local-integrity-and-currentness"},
                   {"project_quality", "not-configured"}, {"gates", Json::array()}};
    try {
        const auto lock = core::lock_index_for_read(options.out_root);
        auto add = [&](const char* name, auto&& invoke) {
            auto detail = core_result(invoke);
            const bool ok = detail.at("ok").template get<bool>();
            result["gates"].push_back({{"id", name}, {"ok", ok}, {"detail", std::move(detail)}});
            return ok;
        };
        bool ok = add("profile-check", [&] { return core::run_profile_check(options, {"--strict"}); });
        ok = add("check", [&] { return core::run_check(options); }) && ok;
        ok = add("doctor", [&] { return core::run_doctor(options, {"--strict"}); }) && ok;
        ok = add("vacuum", [&] { return core::run_vacuum(options, {"--strict"}); }) && ok;
        const auto freshness = core::assess_index_freshness(options, true);
        result["freshness"] = core::index_freshness_json(freshness);
        result["ok"] = ok && freshness.current;
    } catch (const core::CliError& error) {
        result["error"] = {{"code", error.code()}, {"message", error.what()}};
    }
    return result;
}

struct RefreshQualification {
    std::shared_ptr<const core::GenerationAdmission> admission;
    Json validation;
    Json work;
    bool source_cache_transferred = false;
};

std::optional<RefreshQualification> publish_refreshed(const core::RuntimeOptions& options, const Config& config, const fs::path& workspace,
                       const std::string& expected_worktree,
                       const std::function<void()>& verify_authority, LocalPublication* grant = nullptr,
                       const std::optional<SnapshotRepairBase>& repair = {},
                       const std::function<void()>& accept_revision = {},
                       const std::function<void(const std::string&)>& progress = {},
                       const std::string& marker = {}) {
    refresh_progress(progress, "observation");
    const auto inventory = core::observe_worktree(options);
    if (!core::worktree_observation_complete(inventory))
        throw Error("refresh-incomplete-source", "Incomplete source observation cannot replace the active index.");
    if (core::fnv1a64(core::worktree_identity(options, inventory).dump()) != expected_worktree)
        throw Error("refresh-superseded", "Source changed after the coalesced observation.");
    std::set<std::string> counted;
    std::uintmax_t source_bytes = 0;
    for (const auto* files : {&inventory.documents, &inventory.source_facts}) {
        for (const auto& [path, file] : *files) {
            if (!counted.insert(path).second) continue;
            if (file.byte_count > config.max_snapshot_bytes - source_bytes)
                throw Error("refresh-source-budget", "Source exceeds the bounded local refresh budget.");
            source_bytes += file.byte_count;
        }
    }
    const auto output_identity = directory_identity(options.out_root, true);
    std::string base;
    {
        const auto lock = core::lock_index_for_read(options.out_root);
        if (repair) {
            if (inspect_snapshot_repair(config, repair->snapshot) != *repair)
                throw Error("refresh-base-changed", "Damaged publication changed before preparation.");
            base = repair->snapshot;
        } else base = core::load_verified_snapshot(options.out_root, config.max_snapshot_bytes, config.max_file_bytes).snapshot_id;
    }
    LocalStorageReservation storage(workspace.parent_path().parent_path(), options.out_root, 2 * config.max_snapshot_bytes + 512ULL * 1024 * 1024);
    RefreshCandidate candidate(workspace);
    auto prepared_options = options;
    prepared_options.out_root = candidate.path;
    const auto source_fence = [&] { core::require_worktree_stable(options, inventory); };
    refresh_progress(progress, "candidate-build");
    const auto generated = prepare_with_source_fence(verify_authority, source_fence, [&] {
        return core::generate_outputs(options.profile, options.root);
    });
    std::uintmax_t generated_bytes = 0;
    for (const auto& [path, content] : generated.files) {
        (void)path;
        if (content.size() > config.max_file_bytes || content.size() > config.max_snapshot_bytes - generated_bytes)
            throw Error("refresh-output-budget", "Candidate exceeds the local publication budget.");
        generated_bytes += content.size();
    }
    shin::generated_io::publish_snapshot(candidate.path, core::kGeneratedDir, generated.files);
    refresh_progress(progress, "candidate-validation");
    std::vector<std::string> validation_args{"--strict", "--summary"};
    if (grant && !marker.empty()) {
        validation_args.push_back("--marker");
        validation_args.push_back(marker);
    }
    const auto before_validation = grant ? core::generation_fingerprint(prepared_options) : "";
    const auto checked = prepare_with_source_fence(verify_authority, source_fence, [&] {
        return grant ? core_result([&] { return validate_refresh_candidate(prepared_options, config.profile_dir, validation_args, verify_authority, source_fence); }) :
                       bootstrap_readiness(prepared_options);
    });
    if (checked.at("ok") != true) {
        // A verdict on moving input is not a stable quality rejection. Authority
        // changes still take precedence over retryable source observation races.
        verify_authority();
        core::require_worktree_stable(options, inventory);
        throw Error("refresh-validation-failed", "Candidate validation failed; the previous index is preserved.",
                    validation_diagnostic(checked));
    }
    if (grant) {
        prepared_options.generation_admission = core::GenerationAdmission::reuse(prepared_options, before_validation);
        if (!prepared_options.generation_admission)
            throw Error("qualification-inputs-changed", "Candidate dependencies changed during strict validation.");
    }
    refresh_progress(progress, grant ? "candidate-source-cache" : "source-cache");
    std::unique_ptr<core::SourceDiscoveryIndex> prepared_source_index;
    prepare_with_source_fence(verify_authority, source_fence, [&] {
        prepared_source_index = std::make_unique<core::SourceDiscoveryIndex>(prepared_options);
    });
    core::require_worktree_stable(options, inventory);
    verify_authority();
    // Normal publishers share this writer lock. Readers remain on the old
    // generation during preparation; publish_snapshot owns the short index lock.
    refresh_progress(progress, "publication");
    shin::generated_io::IndexLock writer(options.out_root, shin::generated_io::LockMode::exclusive,
                                         shin::generated_io::LockTarget::writer, grant ? grant->token() : "");
    {
        const auto lock = core::lock_index_for_read(options.out_root);
        if (directory_identity(options.out_root, true) != output_identity ||
            (repair ? inspect_snapshot_repair(config, base) != *repair :
                core::load_verified_snapshot(options.out_root, config.max_snapshot_bytes, config.max_file_bytes).snapshot_id != base))
            throw Error("refresh-base-changed", "The active generation changed during preparation.");
    }
    core::require_worktree_stable(options, inventory);
    verify_authority();
    const auto candidate_snapshot = grant ? core::load_verified_snapshot(candidate.path, config.max_snapshot_bytes, config.max_file_bytes).snapshot_id : "";
    if (accept_revision) accept_revision();
    if (grant) grant->preparing(base, candidate_snapshot,
        accept_revision && grant->approved_profile_hash() != options.profile.config_hash ? options.profile.config_hash : "");
    shin::generated_io::publish_snapshot(options.out_root, core::kGeneratedDir, generated.files);
    if (grant) grant->committed(candidate_snapshot);
    if (!grant) return std::nullopt;
    // Transfer only the exact validated byte/dependency closure. The destination
    // remains protected by its writer fence; neither this proof nor its hash grants rights.
    const auto source_lock = core::lock_index_for_read(candidate.path);
    const auto destination_lock = core::lock_index_for_read(options.out_root);
    auto admission = prepare_with_source_fence(verify_authority, source_fence, [&] {
        return prepared_options.generation_admission->published_copy(prepared_options, options);
    });
    if (core::load_verified_snapshot(options.out_root, config.max_snapshot_bytes, config.max_file_bytes).snapshot_id != candidate_snapshot)
        throw Error("query-snapshot-changed", "Published head differs from its strictly validated candidate.");
    auto published_options = options;
    published_options.generation_admission = admission;
    refresh_progress(progress, "source-cache-transfer");
    prepare_with_source_fence(verify_authority, source_fence, [&] {
        prepared_source_index->persist_published_cache(published_options, workspace / "source-cache");
    });
    const auto metrics = prepared_source_index->preparation_metrics();
    Json work{{"source_files", metrics.at("files")}, {"source_bytes", metrics.at("source_bytes")},
        {"source_chunks", metrics.at("chunks")}, {"source_cache_builds", metrics.at("index_builds")},
        {"source_read_scan_ms", metrics.at("read_scan_ms")}, {"source_chunking_ms", metrics.at("chunking_ms")},
        {"source_index_ms", metrics.at("sql_index_ms")}, {"source_persist_ms", metrics.at("persist_ms")}};
    for (const auto* key : {"transfer_ms", "transfer_verify_ms", "transfer_snapshot_ms"})
        work[std::string("source_") + key] = metrics.at(key);
    for (const auto* key : {"generation_runs", "reuses", "verified_source_files", "verified_source_bytes"})
        if (checked.contains("preparation")) work[std::string("validation_") + key] = checked.at("preparation").at(key);
    for (const auto& gate : checked.value("gates", Json::array()))
        if (gate.value("attempted", false) && gate.contains("elapsed_ms"))
            work["validation_" + gate.at("id").get<std::string>() + "_ms"] = gate.at("elapsed_ms");
    verify_authority();
    return RefreshQualification{std::move(admission), checked, work, true};
}

void verify_binding(const Json& binding, const fs::path& root, const std::string& identity,
                    const fs::path& workspace) {
    const std::set<std::string> fields = {"schema_version", "root", "root_identity", "workspace_identity",
        "profile_dir", "profile_config_hash", "core_version", "generator_version", "prepared_snapshot"};
    if (!binding.is_object() || binding.size() != fields.size())
        throw Error("binding-invalid", "Local binding schema is invalid; preserve it for review.");
    for (const auto& key : fields)
        if (!binding.contains(key) || !binding.at(key).is_string())
            throw Error("binding-invalid", "Local binding fields are invalid.");
    if (binding.at("schema_version") != "shin.local-project-binding.v1" ||
        binding.at("root") != root.string() || binding.at("root_identity") != identity ||
        binding.at("workspace_identity") != directory_identity(workspace, true))
        throw Error("binding-identity-changed", "Project directory or private binding identity changed.");
    const fs::path registry = binding.at("profile_dir").get<std::string>();
    if (!registry.is_absolute() || registry.lexically_normal() != registry || fs::canonical(registry) != registry)
        throw Error("binding-invalid", "Bound profile registry is not an existing canonical directory.");
}

Json observer_configuration(const fs::path& workspace, const fs::path& root, const fs::path& registry) {
    const auto id = digest(root.string()).substr(0, 48);
    return {{"schema_version", "shin.observer-config.v2"}, {"observer_id", "observer-" + id},
        {"source_host_id", "offline-local"}, {"project_id", "project-" + id}, {"workspace_id", "workspace-" + id},
        {"root", root.string()}, {"profile", "generic-source"}, {"profile_dir", registry.string()},
        {"out_dir", (workspace / "index").string()}, {"state_dir", (workspace / "observer").string()},
        {"poll_interval_ms", 1000}, {"max_snapshot_bytes", 256U * 1024U * 1024U},
        {"max_file_bytes", 64U * 1024U * 1024U},
        {"outbox", {{"enabled", false}, {"max_entries", 128}, {"max_bytes", 192 * 1024}}}};
}

void add_query_context(Json& result, const core::RuntimeOptions& options, const Config& config,
                       const std::string& query, const fs::path& cache = {}) {
    auto brief = core_result([&] { return core::run_brief(options,
        {"--projection", "compact", "--limit", "4", "--require-current", "--", query}); });
    const auto source = query_source(config, query, true, cache, options.generation_admission);
    const auto snapshot = source.at("source_context").at("snapshot_id");
    const auto freshness = core::assess_index_freshness(options, true);
    core::require_current_index_or_throw(freshness);
    if (brief.at("snapshot_id") != snapshot || freshness.snapshot_id != snapshot.get<std::string>())
        throw Error("query-snapshot-changed", "Publication changed during the query.");
    result["source_context"] = source_view(source);
    result["snapshot_id"] = snapshot;
    result["document_context"] = std::move(brief);
    result["answer_complete"] = false;
    result["query_persisted"] = false;
}

void add_followup_context(Json& result, const Config& config, const LocalProjectRequest& request,
                          const core::RuntimeOptions& options,
                          const Json& prepared_snapshot) {
    if (request.snapshot != prepared_snapshot.get<std::string>())
        throw Error("source-snapshot-mismatch", "Follow-up must use the current prepared snapshot returned by query.");
    if (request.command == "inspect") {
        const auto source = inspect_source(config, request.path, request.snapshot, options.generation_admission);
        result["source_inventory"] = parse_json(source_view(source));
    } else {
        const bool file = request.record.empty();
        const Json selection = {{"schema_version", file ? "shin.source-read-request.v2" : "shin.source-read-request.v1"},
            {"snapshot_id", request.snapshot},
            {file ? "file_paths" : "record_ids", Json::array({file ? request.path : request.record})}};
        const auto source = read_source(config, selection.dump(), options.generation_admission);
        // Explicit reading has its own bounded view; omissions remain visible.
        result["source_context"] = source_view(source, 32768);
        result["native_read_complete"] = source.at("source_read").at("complete");
    }
    result["snapshot_id"] = request.snapshot;
    result["answer_complete"] = false;
    result["query_persisted"] = false;
}

bool attached_binding(const std::optional<Json>& binding) {
    return binding && binding->is_object() &&
           binding->value("schema_version", Json(nullptr)) == "shin.local-route-binding.v1";
}

void require_route_separation(const Config& config, const fs::path& file,
                              const fs::path& root, const fs::path& home) {
    if (config.root != root) throw Error("route-root-mismatch", "Observer configuration belongs to another project root.");
    for (const auto& path : {config.profile_dir, config.out_dir, config.state_dir, file})
        if (overlaps(home, path)) throw Error("unsafe-home", "CLI state must be separate from every canonical route path.");
}

Json marker_identity(const Json& document) {
    return {{"project_id", document.at("project").at("id")},
        {"root_policy", document.at("project").at("rootPolicy")},
        {"profile", document.at("profile")}, {"index", document.at("index")},
        {"agent", document.at("agent")}, {"quality_gates", document.at("qualityGates")}};
}

std::string marker_route_key(Json binding) {
    for (const auto* key : {"prepared_snapshot", "core_version", "generator_version"}) binding.erase(key);
    return digest(binding.dump());
}

bool configuration_approved(const fs::path& workspace, const Json& binding, const std::string& hash,
                            bool allow_empty = false) {
    if (binding.at("observer_config_hash") == hash) return true;
    const auto directory = workspace / "config-policy";
    if (!present(directory)) return false;
    StateStore reader(directory, false);
    const auto saved = reader.load();
    if (!saved && allow_empty) {
        for (const auto& entry : fs::directory_iterator(directory))
            if (entry.path().filename() != "observer.lock" && entry.path().filename() != "state.pending")
                throw Error("config-identity-conflict", "Uncommitted configuration approval contains unowned entries.");
        return false;
    }
    if (!saved || !saved->is_object() || saved->size() != 3 ||
        saved->value("schema_version", Json(nullptr)) != "shin.local-config-policy.v1" ||
        saved->value("route_key", Json(nullptr)) != marker_route_key(binding) ||
        !saved->contains("approved_config_hash") || !saved->at("approved_config_hash").is_string())
        throw Error("config-identity-conflict", "Configuration approval is not bound to this route.");
    const auto approved = saved->at("approved_config_hash").get<std::string>();
    if (approved.size() != 64 || approved.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw Error("config-identity-conflict", "Configuration approval revision is invalid.");
    return approved == hash;
}

void verify_marker_identity(const fs::path& workspace, const Json& binding, const std::string& text,
                            bool capture_original) {
    const auto identity = marker_identity(parse_json(text));
    const auto directory = workspace / "marker-policy";
    const auto key = marker_route_key(binding);
    std::optional<Json> saved;
    if (present(directory)) {
        StateStore reader(directory, false);
        saved = reader.load();
        if (saved && (!saved->is_object() || saved->size() != 4 ||
            saved->value("schema_version", Json(nullptr)) != "shin.local-marker-policy.v1" ||
            saved->value("route_key", Json(nullptr)) != key ||
            saved->value("original_marker_hash", Json(nullptr)) != binding.at("marker_hash")))
            throw Error("marker-identity-conflict", "Original marker policy is not bound to this route.");
        if (!saved) for (const auto& entry : fs::directory_iterator(directory))
            if (entry.path().filename() != "observer.lock" && entry.path().filename() != "state.pending")
                throw Error("marker-identity-conflict", "Uncommitted marker policy contains unowned entries.");
    }
    if (saved) {
        if (saved->at("identity") != identity)
            throw Error("marker-identity-conflict", "Marker project, profile, index, entry or gates changed.");
        return;
    }
    // Old bindings contain only a hash, not the original marker's identity.
    // Capture it only from matching original bytes; never infer it from a revision.
    if (!capture_original || binding.at("marker_hash") != digest(text))
        throw Error("marker-identity-unavailable", "Approve or reconnect the unchanged original marker before revising it.");
    ensure_private_directory(directory);
    StateStore writer(directory, true);
    writer.recover_pending();
    if (writer.load()) throw Error("marker-identity-conflict", "Marker policy changed during capture.");
    writer.save({{"schema_version", "shin.local-marker-policy.v1"}, {"route_key", key},
        {"original_marker_hash", binding.at("marker_hash")}, {"identity", identity}});
}


bool damaged_derived_content(const Error& error) {
    // StateStore has already checked ownership, links and bounds before these
    // content errors. Never reinterpret an unsafe path or I/O failure as a miss.
    return error.code == "invalid-json" || error.code == "json-too-large" || error.code == "state-too-large";
}

std::optional<Json> load_qualification(const fs::path& workspace) {
    const auto directory = workspace / "qualification";
    if (!present(directory)) return std::nullopt;
    StateStore cache(directory, false);
    auto saved = cache.load();
    if (!saved) return std::nullopt;
    const auto hex = [](const Json& value, std::size_t size) {
        return value.is_string() && value.get_ref<const std::string&>().size() == size &&
            value.get_ref<const std::string&>().find_first_not_of("0123456789abcdef") == std::string::npos;
    };
    if (!saved->is_object() || saved->size() != 8 ||
        saved->value("schema_version", Json(nullptr)) != "shin.local-qualification.v2" ||
        saved->value("policy", Json(nullptr)) != "verified-generation-live-safety-v2" ||
        !saved->contains("generation_fingerprint") || !saved->at("generation_fingerprint").is_string() ||
        !saved->at("generation_fingerprint").get_ref<const std::string&>().starts_with("fnv1a64:") ||
        !hex(saved->at("generation_fingerprint").get<std::string>().substr(8), 16) ||
        !saved->contains("key") || !hex(saved->at("key"), 64) ||
        !saved->contains("snapshot_id") || !saved->at("snapshot_id").is_string() ||
        saved->at("snapshot_id").get_ref<const std::string&>().size() != 24 ||
        !saved->at("snapshot_id").get_ref<const std::string&>().starts_with("fnv1a64:") ||
        !hex(saved->at("snapshot_id").get<std::string>().substr(8), 16) ||
        !saved->contains("manifest_sha256") || !hex(saved->at("manifest_sha256"), 64) ||
        !saved->contains("qualified_at_ms") || !saved->at("qualified_at_ms").is_number_integer() ||
        saved->at("qualified_at_ms") <= 0 ||
        !saved->contains("resource") || !saved->at("resource").is_object() ||
        saved->at("resource").size() != 3 || saved->at("resource").dump().size() > 8192)
        throw Error("qualification-invalid", "Stored quality qualification must be recomputed.");
    const auto& resource = saved->at("resource");
    if (!resource.contains("resource_check") || !resource.at("resource_check").is_object() ||
        !resource.contains("warnings") || !resource.at("warnings").is_array() || resource.at("warnings").size() > 8 ||
        !resource.contains("warning_budget") || !resource.at("warning_budget").is_object())
        throw Error("qualification-invalid", "Stored resource diagnostics are invalid.");
    return saved;
}

Json qualified_readiness(core::RuntimeOptions& options, const std::vector<std::string>& args,
                         const Config& config, const fs::path& workspace, const Json& binding,
                         const std::string& key,
                         const std::optional<RefreshQualification>& candidate = {}, RefreshMeasurements* metrics = nullptr) {
    RefreshMeasurements local;
    auto& m = metrics ? *metrics : local;
    options.generation_admission.reset();
    std::optional<Json> saved;
    std::optional<core::VerifiedSnapshot> observed;
    bool invalid = false;
    if (candidate) {
        candidate->admission->verify_content(options);
        observed = m.measure("qualification_snapshot_ms", [&] { return core::load_verified_snapshot(config.out_dir, config.max_snapshot_bytes, config.max_file_bytes); });
        const auto& snapshot = *observed;
        Json resource = {{"resource_check", Json::object()}, {"warnings", Json::array()}, {"warning_budget", Json::object()}};
        for (const auto* field : {"resource_check", "warnings", "warning_budget"})
            if (candidate->validation.contains(field)) resource[field] = candidate->validation.at(field);
        saved = Json{{"key", key}, {"snapshot_id", snapshot.snapshot_id},
            {"manifest_sha256", digest(snapshot.manifest_content)},
            {"generation_fingerprint", candidate->admission->fingerprint()},
            {"qualified_at_ms", now_ms()}, {"resource", std::move(resource)}};
    }
    try { if (!candidate) saved = load_qualification(workspace); }
    catch (const Error& error) {
        if (!damaged_derived_content(error) && error.code != "qualification-invalid") throw;
        invalid = true;
    }
    if (saved && saved->at("key") == key) {
        // Only a private qualification for this exact kit, route and byte-level
        // generation can replace regeneration. Safety remains a live check.
        if (!observed) observed = m.measure("qualification_snapshot_ms", [&] { return core::load_verified_snapshot(config.out_dir, config.max_snapshot_bytes, config.max_file_bytes); });
        const auto& snapshot = *observed;
        if (saved->at("snapshot_id") == snapshot.snapshot_id &&
            saved->at("manifest_sha256") == digest(snapshot.manifest_content)) {
            options.generation_admission = m.measure("qualification_generation_ms", [&] { return core::GenerationAdmission::reuse(options, saved->at("generation_fingerprint"), &snapshot); });
          if (options.generation_admission) {
            m.values["qualification_reused_output_bytes"] = options.generation_admission->reused_output_bytes();
            m.values["qualification_reused_output_files"] = options.generation_admission->reused_output_files();
            auto checks = Json::array({{{"id", "verified-generation"}, {"ok", true}}});
            const auto check = [&](const char* id, const char* metric, auto&& invoke) {
                auto value = m.measure(metric, [&] { return core_result(invoke); });
                checks.push_back({{"id", id}, {"ok", value.at("ok")}});
                return value.at("ok").template get<bool>();
            };
            bool ok = check("profile-check", "qualification_profile_ms", [&] { return core::run_profile_check(options, {"--strict"}); });
            ok = check("doctor", "qualification_doctor_ms", [&] { return core::run_doctor(options, {"--strict"}); }) && ok;
            if (binding.value("marker", Json("")) != "") {
                const auto adopted = m.measure("qualification_adoption_ms", [&] { return core_result([&] { return core::run_adoption_check(options.root, config.profile_dir,
                    {"--marker", binding.at("marker").get<std::string>(), "--strict"}); }); });
                const bool aligned = adopted.at("ok") == true && adopted.at("profile").at("id") == options.profile.id &&
                    adopted.at("index").at("key") == options.out_root.filename().string();
                checks.push_back({{"id", "adoption-check"}, {"ok", aligned}});
                ok = aligned && ok;
            }
            if (ok) return {{"schema_version", "shin.local-qualified-readiness.v1"}, {"ok", true}, {"ready", true},
                {"freshness", core::index_freshness_json(options.generation_admission->assess(options))},
                {"snapshot_id", snapshot.snapshot_id}, {"diagnostic_ids", Json::array()}, {"live_checks", checks},
                {"quality_reuse", {{"reused", !candidate.has_value()}, {"cache_status", candidate ? "published-candidate" : "hit"}, {"scope", "exact-qualified-generation"},
                    {"qualified_at_ms", saved->at("qualified_at_ms")}}},
                {"qualified_resources", saved->at("resource")},
                {"summary", {{"attempted", checks.size()}, {"passed", checks.size()}, {"failed", 0},
                    {"validation_passed", true}, {"worktree_current", true}}}};
          }
        }
    }
    options.generation_admission.reset();
    if (options.validation_checkpoint) options.validation_checkpoint();
    std::string before;
    std::exception_ptr capture_error;
    try { before = core::generation_fingerprint(options); }
    catch (const core::CliError&) { capture_error = std::current_exception(); }
    auto result = options.profile.bootstrap_only ? bootstrap_readiness(options) :
        core_result([&] { return core::run_ready(options, config.profile_dir, args); });
    if (options.profile.bootstrap_only) result["ready"] = result.at("ok");
    if (result.at("ready") == true) {
        if (capture_error) std::rethrow_exception(capture_error);
        options.generation_admission = core::GenerationAdmission::reuse(options, before);
        if (!options.generation_admission)
            throw Error("qualification-inputs-changed", "Generation inputs changed during full qualification.");
    }
    result["quality_reuse"] = {{"reused", false}, {"scope", "exact-qualified-generation"},
        {"cache_status", invalid ? "invalid" : saved ? "miss" : "missing"}};
    return result;
}

void save_qualification(const fs::path& workspace, const Json& binding, const Config& config,
                        const std::string& snapshot, const Json& readiness, const std::string& key,
                        const core::RuntimeOptions& options,
                        const std::optional<std::string>& marker_hash = std::nullopt) {
    if (!options.generation_admission)
        throw Error("qualification-missing", "No generation was admitted by the completed qualification.");
    options.generation_admission->verify_content(options);
    const auto verified = core::load_verified_snapshot(config.out_dir, config.max_snapshot_bytes, config.max_file_bytes);
    if (verified.snapshot_id != snapshot)
        throw Error("query-snapshot-changed", "Publication changed before saving its quality qualification.");
    const auto directory = workspace / "qualification";
    ensure_private_directory(directory);
    StateStore cache(directory, true);
    cache.recover_pending();
    Json resource = {{"resource_check", Json::object()}, {"warnings", Json::array()}, {"warning_budget", Json::object()}};
    if (readiness.contains("qualified_resources")) resource = readiness.at("qualified_resources");
    for (const auto& step : readiness.value("steps", Json::array())) {
        if (step.at("id") != "validate" || !step.at("result").is_object()) continue;
        for (const auto* field : {"resource_check", "warnings", "warning_budget"})
            if (step.at("result").contains(field)) resource[field] = step.at("result").at(field);
    }
    if (qualification_key(binding, options.profile.config_hash, marker_hash) != key)
        throw Error("qualification-engine-changed", "The executable changed while qualifying this snapshot; restart with one matched kit.");
    cache.save({{"schema_version", "shin.local-qualification.v2"}, {"key", key},
        {"generation_fingerprint", options.generation_admission->fingerprint()},
        {"snapshot_id", snapshot}, {"manifest_sha256", digest(verified.manifest_content)}, {"qualified_at_ms", now_ms()},
        {"policy", "verified-generation-live-safety-v2"}, {"resource", std::move(resource)}});
}

Json attached_project(const LocalProjectRequest& request, const fs::path& root, const fs::path& home,
                      const fs::path& workspace, StateStore& store, const std::optional<Json>& loaded,
                      const LocalProjectLifecycle& lifecycle) {
    const bool attach = request.command == "attach" || request.command == "init";
    const bool refresh = request.command == "_refresh";
    if (loaded) {
        const std::set<std::string> fields = {"schema_version", "root", "root_identity", "workspace_identity",
            "observer_config", "observer_config_hash", "observer_binding_key", "profile_config_hash",
            "index_identity", "marker", "marker_hash", "core_version", "generator_version", "prepared_snapshot"};
        if (!attached_binding(loaded) || loaded->size() != fields.size())
            throw Error("binding-mode-conflict", "A different binding already exists; automatic replacement is disabled.");
        for (const auto& key : fields)
            if (!loaded->contains(key) || !loaded->at(key).is_string())
                throw Error("binding-invalid", "Attached route binding fields are invalid.");
        if (loaded->at("root") != root.string() || loaded->at("root_identity") != directory_identity(root) ||
            loaded->at("workspace_identity") != directory_identity(workspace, true))
            throw Error("binding-identity-changed", "Attached directory identity changed.");
    }
    if (attach) check_local_identity(home,root);
    fs::path file;
    if (loaded) file = loaded->at("observer_config").get<std::string>();
    else if (request.observer_config) file = fs::absolute(*request.observer_config).lexically_normal();
    else throw Error("invalid-arguments", "attach requires an Observer configuration.");
    if (request.observer_config && fs::absolute(*request.observer_config).lexically_normal() != file)
        throw Error("route-config-conflict", "An attached project cannot silently switch Observer configurations.");
    const auto config_text = read_text(file, 65536, true);
    const auto config = config_from_json(parse_json(config_text), file);
    require_route_separation(config, file, root, home);
    const auto conventional = core::find_existing_adoption_marker(root);
    const auto supplied = request.marker ? std::optional(core::normalize_repository_file_argument(root, *request.marker, "--marker")) : std::nullopt;
    const std::string marker = loaded ? loaded->at("marker").get<std::string>() : supplied.value_or(conventional.value_or(""));
    if ((supplied && *supplied != marker) || (conventional && *conventional != marker))
        throw Error("route-marker-conflict", "The selected marker differs from the canonical route marker.");
    const auto marker_hash = marker.empty() ? "" : digest(read_text(root / marker, 1024 * 1024, false));
    core::RuntimeOptions options;
    options.root = root;
    options.out_root = config.out_dir;
    options.profile_dir = config.profile_dir;
    options.profile = core::make_profile(config.profile, config.profile_dir);
    if (options.profile.bootstrap_only)
        throw Error("route-not-permanent", "Attach requires a permanent profile with its original quality gates.");
    Json binding = {{"schema_version", "shin.local-route-binding.v1"}, {"root", root.string()},
        {"root_identity", directory_identity(root)}, {"workspace_identity", directory_identity(workspace, true)},
        {"observer_config", file.string()}, {"observer_config_hash", digest(config_text)},
        {"observer_binding_key", config.binding_key}, {"profile_config_hash", options.profile.config_hash},
        {"index_identity", directory_identity(config.out_dir)}, {"marker", marker}, {"marker_hash", marker_hash},
        {"core_version", SHIN_VERSION}, {"generator_version", core::kGeneratorVersion}, {"prepared_snapshot", ""}};
    if (loaded) {
        for (const auto* key : {"observer_binding_key", "index_identity"})
            if (loaded->at(key) != binding.at(key))
                throw Error("route-revision-conflict", "Canonical policy or route identity changed; review is required, not automatic reattachment.");
        if (request.command == "config-accept") {
            if (request.config_revision.size() != 64 ||
                request.config_revision.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw Error("invalid-config-revision", "Use the exact lowercase SHA256 configuration revision.");
            if (request.config_revision != binding.at("observer_config_hash").get<std::string>())
                throw Error("config-revision-changed", "The reviewed configuration revision is no longer current.");
            // The binding key pins all paths, identities and outbox policy. Only
            // operational bounds/polling can change, with publication revoked.
            LocalPublication previous(workspace, *loaded, config);
            if (previous.phase() != "read-only" && previous.phase() != "revoked")
                throw Error("config-requires-release", "Run refresh-release before approving operational settings.");
            if (previous.approved_profile_hash() != options.profile.config_hash ||
                previous.approved_marker_hash() != marker_hash)
                throw Error("route-revision-conflict", "Configuration approval cannot change profile or marker policy.");
            const bool changed = !configuration_approved(workspace, *loaded, request.config_revision, true);
            if (store.load() != loaded || read_text(file, 65536, true) != config_text ||
                core::make_profile(config.profile, config.profile_dir).config_hash != options.profile.config_hash ||
                core::find_existing_adoption_marker(root) != conventional ||
                (!marker.empty() && digest(read_text(root / marker, 1024 * 1024, false)) != marker_hash) ||
                directory_identity(root) != binding.at("root_identity").get<std::string>() ||
                directory_identity(workspace, true) != binding.at("workspace_identity").get<std::string>() ||
                directory_identity(config.out_dir) != binding.at("index_identity").get<std::string>() ||
                directory_identity(config.state_dir, true) != config.binding.at("state_identity").get<std::string>() ||
                LocalPublication(workspace, *loaded, config).phase() != previous.phase())
                throw Error("refresh-authority-changed", "Route changed before configuration approval.");
            if (changed) {
                const auto directory = workspace / "config-policy";
                ensure_private_directory(directory);
                StateStore approval(directory, true);
                approval.recover_pending();
                approval.save({{"schema_version", "shin.local-config-policy.v1"},
                    {"route_key", marker_route_key(*loaded)}, {"approved_config_hash", request.config_revision}});
            }
            auto result = envelope(request.command, true);
            result.update({{"original_config_hash", loaded->at("observer_config_hash")}, {"approved_config_hash", request.config_revision},
                {"changed", changed}, {"local_ready", false}, {"readiness_assessed", false},
                {"index_mutated", false}, {"data_preserved", true}, {"next_action", "shin-ce init"}});
            return result;
        }
        if (!configuration_approved(workspace, *loaded, binding.at("observer_config_hash")))
            throw Error("route-revision-conflict", "Canonical configuration changed; explicit config-accept after refresh-release is required.");
        binding = *loaded;
    }
    const bool readable_binding = binding.at("generator_version") == core::kGeneratorVersion ||
        binding.at("generator_version") == core::kLegacyGeneratorVersion;
    if (refresh && (!loaded || !readable_binding))
        throw Error("refresh-engine-changed", "Review the installed kit and run init once after an engine upgrade.");
    LocalPublication grant(workspace, binding, config, request.manage_index);
    if (!grant.active() && grant.phase() != "read-only" && grant.phase() != "revoked" && !request.manage_index)
        throw Error("refresh-grant-incomplete", "Finish managed init or refresh-release before using this route.");
    if (grant.active()) grant.verify();
    if (request.command == "marker-accept") {
        if (!loaded || !grant.active()) throw Error("refresh-grant-required", "Marker approval requires this route's active publisher grant.");
        if (request.marker_revision.size() != 64 ||
            request.marker_revision.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw Error("invalid-marker-revision", "Use the exact lowercase SHA256 marker revision.");
        if (marker.empty() || request.marker_revision != marker_hash)
            throw Error("marker-revision-changed", "The requested marker revision is not the current canonical revision.");
        if (grant.approved_profile_hash() != options.profile.config_hash)
            throw Error("route-revision-conflict", "Approve the canonical profile separately before marker approval.");
        const auto checked = core_result([&] { return core::run_adoption_check(root, config.profile_dir,
            {"--marker", marker, "--strict"}); });
        if (checked.at("ok") != true || checked.at("profile").at("id") != config.profile ||
            checked.at("index").at("key") != config.out_dir.filename().string())
            throw Error("invalid-marker-revision", "Strict adoption checks or canonical profile/index alignment failed.");
        const auto marker_text = read_text(root / marker, 1024 * 1024, false);
        if (store.load() != loaded || read_text(file, 65536, true) != config_text || digest(marker_text) != marker_hash ||
            core::make_profile(config.profile, config.profile_dir).config_hash != options.profile.config_hash ||
            core::find_existing_adoption_marker(root) != conventional ||
            directory_identity(root) != binding.at("root_identity").get<std::string>() ||
            directory_identity(workspace, true) != binding.at("workspace_identity").get<std::string>() ||
            directory_identity(config.state_dir, true) != config.binding.at("state_identity").get<std::string>())
            throw Error("refresh-authority-changed", "Route authority changed before marker approval.");
        verify_marker_identity(workspace, binding, marker_text, true);
        const auto previous = grant.approved_marker_hash();
        grant.accept_marker(request.marker_revision);
        auto result = envelope(request.command, true);
        result.update({{"profile", config.profile}, {"previous_marker_hash", previous},
            {"approved_marker_hash", request.marker_revision}, {"changed", previous != request.marker_revision},
            {"local_ready", false}, {"readiness_assessed", false}, {"index_mutated", false},
            {"data_preserved", true}, {"next_action", "wait-for-local-refresh"}});
        return result;
    }
    std::optional<std::string> reviewed_profile_text;
    if (request.command == "profile-review" ||
        (request.command == "profile-accept" && !request.profile_review_token.empty())) {
        if (!loaded || !grant.active()) throw Error("refresh-grant-required", "Profile review requires the existing publisher grant.");
        if (grant.approved_marker_hash() != marker_hash)
            throw Error("route-revision-conflict", "Marker changes require a separate review.");
        LocalProfileRevision revision(workspace, binding, grant);
        const auto approved_hash = grant.approved_profile_hash();
        const auto approved = revision.approved(approved_hash);
        const auto text = read_text(options.profile.config_source, 1024 * 1024, false);
        if (core::fnv1a64(text) != options.profile.config_hash)
            throw Error("profile-review-changed", "Profile changed; review the new revision.");
        reviewed_profile_text = text;
        auto review = profile_review_summary(approved, text);
        const auto checked = core_result([&] { return core::run_profile_check(options, {"--strict"}); });
        review["valid"] = checked.at("ok");
        if (checked.at("ok") != true) review["can_accept"] = false;
        review["revision"] = options.profile.config_hash;
        // Bind consent to exact bytes AND existing authority, not only the
        // compatibility FNV revision. Never expose the publisher token itself.
        review["review_token"] = digest(Json::array({grant.token(), binding, config_text, marker_hash,
            approved_hash, approved.value_or(""), text}).dump());
        if (store.load() != loaded || read_text(file, 65536, true) != config_text ||
            read_text(options.profile.config_source, 1024 * 1024, false) != text ||
            (!marker.empty() && digest(read_text(root / marker, 1024 * 1024, false)) != marker_hash))
            throw Error("profile-review-changed", "Route changed; review the new revision.");
        grant.verify();
        if (directory_identity(root) != binding.at("root_identity").get<std::string>() ||
            directory_identity(workspace, true) != binding.at("workspace_identity").get<std::string>() ||
            directory_identity(config.out_dir) != binding.at("index_identity").get<std::string>())
            throw Error("profile-review-changed", "Project identity changed during review.");
        if (request.command == "profile-review") {
            auto result = envelope(request.command, true);
            result.update({{"root", root.string()}, {"profile_review", std::move(review)},
                {"index_mutated", false}, {"readiness_assessed", false}});
            return result;
        }
        if (!review.at("can_accept").get<bool>() || request.profile_review_token != review.at("review_token").get<std::string>())
            throw Error("profile-review-changed", "The reviewed changes are no longer eligible; review again.");
    }
    if (request.command == "profile-accept") {
        if (!loaded || !grant.active()) throw Error("refresh-grant-required", "Profile approval requires this route's active publisher grant.");
        if (grant.approved_marker_hash() != marker_hash)
            verify_marker_identity(workspace, binding, read_text(root / marker, 1024 * 1024, false), false);
        if (request.profile_revision != options.profile.config_hash)
            throw Error("profile-revision-changed", "The requested profile revision is not the current canonical revision.");
        const auto checked = core_result([&] { return core::run_profile_check(options, {"--strict"}); });
        if (checked.at("ok") != true) throw Error("invalid-profile-revision", "Profile configuration checks failed.");
        const auto accepted_text = read_text(options.profile.config_source, 1024 * 1024, false);
        if ((reviewed_profile_text && *reviewed_profile_text != accepted_text) ||
            core::fnv1a64(accepted_text) != request.profile_revision)
            throw Error("profile-revision-changed", "Profile changed during approval.");
        // Approving policy never recovers or changes a publication. Preserve
        // any pending snapshot intent for the normal refresh/recovery path.
        if (store.load() != loaded || read_text(file, 65536, true) != config_text ||
            (!marker.empty() && digest(read_text(root / marker, 1024 * 1024, false)) != marker_hash) ||
            directory_identity(root) != binding.at("root_identity").get<std::string>() ||
            directory_identity(workspace, true) != binding.at("workspace_identity").get<std::string>() ||
            directory_identity(config.out_dir) != binding.at("index_identity").get<std::string>() ||
            read_text(options.profile.config_source, 1024 * 1024, false) != accepted_text)
            throw Error("refresh-authority-changed", "Route authority changed before profile approval.");
        const auto previous = grant.approved_profile_hash();
        LocalProfileRevision revision(workspace, binding, grant);
        if (accepted_text.size() <= 96 * 1024) revision.remember(previous, accepted_text);
        grant.accept_profile(request.profile_revision);
        auto result = envelope(request.command, true);
        result.update({{"profile", config.profile}, {"previous_profile_hash", previous},
            {"approved_profile_hash", request.profile_revision}, {"changed", previous != request.profile_revision},
            {"local_ready", false}, {"readiness_assessed", false}, {"index_mutated", false},
            {"data_preserved", true}, {"next_action", "wait-for-local-refresh"}});
        return result;
    }
    if (grant.approved_marker_hash() != marker_hash)
        throw Error("route-revision-conflict", "Canonical marker revision changed; explicit marker-accept is required.");
    const auto require_running = [&] {
        if (refresh && request.maintenance_stop && request.maintenance_stop())
            throw Error("refresh-stopped", "Local preparation was stopped.");
    };
    require_running();
    const auto managed_authority = [&] {
        require_running();
        grant.verify();
        if (store.load() != loaded || read_text(file, 65536, true) != config_text ||
            core::make_profile(config.profile, config.profile_dir).config_hash != options.profile.config_hash ||
            core::find_existing_adoption_marker(root) != conventional ||
            (!marker.empty() && digest(read_text(root / marker, 1024 * 1024, false)) != marker_hash) ||
            directory_identity(root) != binding.at("root_identity").get<std::string>() ||
            directory_identity(workspace, true) != binding.at("workspace_identity").get<std::string>())
            throw Error("refresh-authority-changed", "Granted configuration, policy or directory changed before publication.");
    };
    const auto repair = refresh && grant.active() ? (managed_authority(), grant.reconcile()) : std::optional<SnapshotRepairBase>{};
    LocalProfileRevision revision(workspace, binding, grant);
    const auto profile_text = read_text(options.profile.config_source, 1024 * 1024, false);
    if (core::fnv1a64(profile_text) != options.profile.config_hash)
        throw Error("profile-revision-changed", "Profile changed during admission.");
    auto approved_text = grant.active() ? revision.approved(grant.approved_profile_hash()) : std::nullopt;
    bool automatic_revision = grant.approved_profile_hash() != options.profile.config_hash;
    if (automatic_revision && (!grant.active() || !approved_text ||
        !safe_profile_membership_change(root, *approved_text, profile_text) ||
        core::inspect_profile_policy(root, config.profile_dir, config.profile, options.profile.config_hash).at("ok") != true))
        throw Error("route-revision-conflict", "Protected profile policy changed or its approved baseline is unavailable; explicit profile-accept is required.");
    if (!automatic_revision && refresh && grant.active() && !approved_text && profile_text.size() <= 96 * 1024) {
        revision.remember(grant.approved_profile_hash(), profile_text);
        approved_text = profile_text;
    }
    bool missing_members = false;
    if (grant.active() && approved_text && !automatic_revision) {
        for (const auto* members : {&options.profile.root_files, &options.profile.source_fact_roots})
            for (const auto& path : *members) {
                std::error_code error;
                const auto entry = fs::symlink_status(root / path, error);
                if ((!error || error == std::errc::no_such_file_or_directory) && !fs::exists(entry)) missing_members = true;
            }
    }
    bool partial_allowed = automatic_revision || missing_members;
    auto quality_binding = binding;
    quality_binding["observer_config_hash"] = digest(config_text);
    const auto quality_key = qualification_key(quality_binding, options.profile.config_hash, marker_hash);
    // A grant keeps its original route identity. Same-format managed updates
    // qualify the actual new executable separately; they never rewrite a grant
    // merely because the Core build version advanced.
    if ((grant.active() && !readable_binding) ||
        (refresh && !grant.active() && binding.at("core_version") != SHIN_VERSION))
        throw Error("refresh-engine-changed", "The granted index format or external engine binding needs explicit migration.");
    if (!attach && !refresh && !grant.active() && store.writer_present())
        throw Error("project-busy", "Project initialization is in progress.");
    if (refresh) recover_refresh_candidate(workspace);
    bool published = false;
    bool repaired = false;
    std::optional<RefreshQualification> published_qualification;
    Json managed_observation;
    bool source_limit_partial = false;
    if (!refresh && !partial_allowed && approved_text && grant.active() &&
        options.profile.config_hash == grant.approved_profile_hash() && present(workspace / "watch")) {
        managed_observation = retained_source_limit_observation(config, workspace, binding.at("root_identity"), options.profile.config_hash);
        partial_allowed = source_limit_partial = !managed_observation.is_null();
    }
    const auto partial_response = [&](Json diagnostic = Json()) -> Json {
        if (diagnostic.is_null() && !source_limit_partial) diagnostic = revision.rejection(options.profile.config_hash, "");
        if (!partial_allowed || !approved_text || !grant.active())
            throw Error("profile-baseline-invalid", "Partial context requires an approved profile baseline.");
        managed_authority();
        const auto approved_hash = grant.approved_profile_hash();
        const auto prior = core::load_verified_snapshot(config.out_dir, config.max_snapshot_bytes, config.max_file_bytes);
        if (prior.snapshot_id != grant.last_snapshot())
            throw Error("refresh-base-changed", "Retained publication is not the approved publisher head.");
        auto approved_profile = approved_membership_profile(options.profile, *approved_text);
        const auto authority = [config = Config(config), workspace = workspace, binding = Json(binding), root = root, home = home,
                                marker = marker, marker_hash = marker_hash, file = file, config_text = config_text,
                                conventional = conventional, approved_hash = approved_hash,
                                proposed_hash = options.profile.config_hash, lifecycle = lifecycle] {
            StateStore current(workspace, false);
            LocalPublication retained(workspace, binding, config);
            retained.verify();
            if (!retained.active() || retained.approved_profile_hash() != approved_hash ||
                local_project_lifecycle(workspace) != lifecycle ||
                read_text(file, 65536, true) != config_text ||
                !configuration_approved(workspace, binding, digest(config_text)) ||
                core::make_profile(config.profile, config.profile_dir).config_hash != proposed_hash ||
                (!marker.empty() && digest(read_text(root / marker, 1024 * 1024, false)) != marker_hash) ||
                core::find_existing_adoption_marker(root) != conventional || current.load() != std::optional<Json>(binding) ||
                directory_identity(root) != binding.at("root_identity").get<std::string>() ||
                directory_identity(workspace, true) != binding.at("workspace_identity").get<std::string>() ||
                directory_identity(config.out_dir) != binding.at("index_identity").get<std::string>() ||
                directory_identity(config.state_dir, true) != config.binding.at("state_identity").get<std::string>())
                throw Error("route-revision-conflict", "Partial context authority changed.");
            guard_ancestor_bindings(root, home);
        };
        auto result = envelope(request.command, true);
        result.update(retained_context_status(config, prior.snapshot_id, approved_hash, options.profile.config_hash, diagnostic,
            source_limit_partial ? managed_observation : Json()));
        if (!diagnostic.is_null()) result["candidate_diagnostic"] = diagnostic;
        if (refresh) {
            result.update({{"command", "watch-tick"}, {"phase", "partial"}, {"rebuilt", false},
                {"refresh_authority", "local-canonical"}, {"workspace_identity", binding.at("workspace_identity")}});
            if (!managed_observation.is_null()) result["observation"] = managed_observation;
            return result;
        }
        auto reader = std::make_shared<LocalQuerySession>(config, workspace, workspace / "source-cache", result,
                                                       authority, &approved_profile);
        if (request.query_session) *request.query_session = reader;
        return reader->execute(request);
    };
    if (partial_allowed && !refresh) return partial_response();
    if (refresh && grant.active()) {
        const auto& authority = managed_authority;
        authority();
        refresh_stage(request, "observation");
        managed_observation = sample_worktree(config, binding.at("root_identity"), repair.has_value());
        if (!partial_allowed && !repair && approved_text &&
            options.profile.config_hash == grant.approved_profile_hash() && source_observation_limited(managed_observation))
            partial_allowed = source_limit_partial = true;
        auto pending_tick = envelope("watch-tick", true);
        pending_tick.update({{"index", config.out_dir.string()}, {"observation", managed_observation},
            {"rebuilt", false}, {"recovery_pending", repair.has_value()},
            {"refresh_authority", "local-canonical"}, {"workspace_identity", binding.at("workspace_identity")}});
        if (managed_observation.at("status") != "current" && managed_observation.at("status") != "stale" &&
            !(repair && managed_observation.at("status") == "unpublished")) {
            if (partial_allowed) {
                const auto error = source_observation_error(managed_observation);
                return partial_response(error.value("diagnostic", Json{{"schema_version", "shin.validation-diagnostic.v1"},
                    {"gates", Json::array({{{"id", "source-observation"}, {"reasons", Json::array({error.at("code")})}}})}}));
            }
            pending_tick.update({{"ok", false}, {"phase", "blocked"}});
            pending_tick["error"] = source_observation_error(managed_observation);
            return pending_tick;
        }
        if (managed_observation.at("snapshot_current") != true) {
            if (request.maintenance_worktree != managed_observation.at("worktree_id").get<std::string>()) {
                pending_tick["phase"] = "coalescing";
                if (partial_allowed) return partial_response();
                return pending_tick;
            }
            const auto rejected = partial_allowed ? revision.rejection(options.profile.config_hash, request.maintenance_worktree) : Json();
            if (!rejected.is_null()) return partial_response(rejected);
            try {
            published_qualification = publish_refreshed(options, config, workspace, request.maintenance_worktree, authority, &grant, repair, [&] {
                if (!automatic_revision) return;
                managed_authority();
                revision.remember(grant.approved_profile_hash(), profile_text);

            }, request.maintenance_progress, marker);
            automatic_revision = false;
            } catch (const Error& error) {
                if (!automatic_revision && !missing_members) throw;
                managed_authority();
                const auto diagnostic = error.diagnostic.is_null() ? Json{{"schema_version", "shin.validation-diagnostic.v1"},
                    {"gates", Json::array({{{"id", "candidate"}, {"error_code", validation_identifier(error.code)}}})}} : error.diagnostic;
                revision.reject(options.profile.config_hash, request.maintenance_worktree, diagnostic);
                return partial_response(diagnostic);
            } catch (const core::CliError& error) {
                if (!automatic_revision && !missing_members) throw;
                managed_authority();
                const Json diagnostic{{"schema_version", "shin.validation-diagnostic.v1"},
                    {"gates", Json::array({{{"id", "candidate"}, {"error_code", validation_identifier(error.code())}}})}};
                revision.reject(options.profile.config_hash, request.maintenance_worktree, diagnostic);
                return partial_response(diagnostic);
            }
            published = true;
            repaired = repair.has_value();
            managed_observation = sample_worktree(config, binding.at("root_identity"));
        }
    }
    const auto verify_refresh_source = [&] {
        if (!refresh || !grant.active()) return;
        managed_authority();
        const auto current = sample_worktree(config, binding.at("root_identity"));
        if ((current.at("status") == "current" || current.at("status") == "stale") &&
            (current.at("snapshot_current") != true ||
             current.at("worktree_id") != managed_observation.at("worktree_id")))
            throw Error("refresh-superseded", "Source changed during refresh qualification.");
    };
    return prepare_with_source_fence([&] {
        if (refresh && grant.active()) managed_authority();
    }, verify_refresh_source, [&]() -> Json {
    verify_refresh_source();
    std::vector<std::string> args = {"--strict"};
    if (!marker.empty()) args.insert(args.end(), {"--marker", marker});
    // Keep the quality decision, cache preparation and delivered context on one
    // publication, including between Core commands with their own read locks.
    auto publication = core::lock_index_for_read(config.out_dir);
    const auto admission_started = core::SteadyClock::now();
    refresh_stage(request, "qualification");
    RefreshMeasurements qualification_metrics;
    if (refresh && grant.active()) options.validation_checkpoint = verify_refresh_source;
    auto readiness = qualified_readiness(options, args, config, workspace, quality_binding, quality_key, published_qualification, &qualification_metrics);
    options.validation_checkpoint = {};
    const double admission_ms = core::elapsed_ms_since(admission_started);
    const auto observer = observer_status(config);
    const auto& state = observer.at("state");
    const bool sampled = !state.is_null() && state.at("root_identity") == binding.at("root_identity");
    const bool canonical_ready = readiness.at("ready") == true;
    if (!canonical_ready) verify_refresh_source();
    std::string snapshot;
    if (canonical_ready) {
        const auto freshness = core::assess_index_freshness(options, true);
        core::require_current_index_or_throw(freshness);
        snapshot = freshness.snapshot_id;
        if (attach && core::load_verified_snapshot(config.out_dir, config.max_snapshot_bytes,
                                                   config.max_file_bytes).snapshot_id != snapshot)
            throw Error("query-snapshot-changed", "Publication changed during attachment preflight.");
    }
    if ((attach || (refresh && binding.at("prepared_snapshot") != snapshot)) && canonical_ready && sampled) {
        const bool source_cache_transferred = refresh && published_qualification &&
            published_qualification->source_cache_transferred;
        if (!source_cache_transferred) refresh_stage(request, "source-cache");
        if (refresh && grant.active()) {
            const auto source_guard = [&] {
                const auto current = sample_worktree(config, binding.at("root_identity"));
                if (current.at("status") == "stale" && current.at("worktree_id") != managed_observation.at("worktree_id"))
                    throw Error("refresh-superseded", "Source changed during search preparation.");
            };
            if (source_cache_transferred) prepare_with_source_fence(managed_authority, source_guard, [] {});
            else prepare_with_source_fence(managed_authority, source_guard, [&] {
                core::SourceDiscoveryIndex prepared(options, workspace / "source-cache");
            });
        } else {
            core::SourceDiscoveryIndex prepared(options, workspace / "source-cache");
        }
        const auto freshness = core::assess_index_freshness(options, true);
        core::require_current_index_or_throw(freshness);
        if (snapshot != freshness.snapshot_id) throw Error("query-snapshot-changed", "Publication changed during attachment.");
        // Granted route versions are immutable authority identity. Executable
        // and current artifact versions are checked by qualification instead.
        if (!grant.active()) {
            binding["core_version"] = SHIN_VERSION;
            binding["generator_version"] = core::kGeneratorVersion;
        }
        binding["prepared_snapshot"] = snapshot;
    }
    const bool local_ready = canonical_ready && sampled && binding.at("prepared_snapshot") == snapshot &&
        (grant.active() || (binding.at("core_version") == SHIN_VERSION &&
                            binding.at("generator_version") == core::kGeneratorVersion));
    auto result = envelope(request.command, local_ready);
    const auto manifest = canonical_ready ? Json::parse(core::load_verified_snapshot(config.out_dir,
        config.max_snapshot_bytes, config.max_file_bytes).manifest_content) : Json::object();
    const auto coverage = source_coverage_summary(manifest);
    if (!coverage.is_null()) result["source_coverage"] = coverage;
    result.update({{"binding_kind", "existing-route"}, {"local_ready", local_ready}, {"canonical_ready", canonical_ready},
        {"readiness_scope", "selected-canonical-route"}, {"project_quality", canonical_ready ? "declared-checks-passed" : "blocked"},
        {"state", local_ready ? "local-ready" : !canonical_ready ? "blocked-route" : !sampled ? "needs-observer-sample" : "needs-init"},
        {"root", root.string()}, {"index", config.out_dir.string()}, {"profile", config.profile},
        {"project_id", config.binding.at("project_id")}, {"workspace_id", config.binding.at("workspace_id")},
        {"profile_config_hash", options.profile.config_hash}, {"marker", marker.empty() ? Json(nullptr) : Json(marker)},
        {"observer_config", file.string()},
        {"readiness", std::move(readiness)}, {"rebuilt", false}, {"index_mutated", false}, {"observer_state_mutated", false},
        {"synchronization", "not-assessed"}, {"observer_sample_available", sampled},
        {"next_action", local_ready ? "shin-ce query" : !canonical_ready ? "repair-canonical-route" :
                        !sampled ? "sample-canonical-observer" : "shin-ce init"},
        {"refresh_management", grant.active() ? "local-service" : "external"},
        {"publication", grant.active() ? "managed" : grant.phase()}});
    if (attach) result["service_enrolled"] = false;
    const auto retrieval_started = core::SteadyClock::now();
    if (local_ready && request.command == "query")
        add_query_context(result, options, config, request.query, workspace / "source-cache");
    if (local_ready && (request.command == "inspect" || request.command == "read"))
        add_followup_context(result, config, request, options, binding.at("prepared_snapshot"));
    const double retrieval_ms = core::elapsed_ms_since(retrieval_started);
    // Managed readers hold the publication read lock and recheck every binding.
    // An unchanged maintenance writer alone must not invalidate that evidence.
    if (!attach && !refresh && grant.active()) grant.verify();
    const auto verify_route = [&](const std::optional<Json>& expected) {
      if (read_text(file, 65536, true) != config_text ||
        !configuration_approved(workspace, binding, digest(config_text)) ||
        core::make_profile(config.profile, config.profile_dir).config_hash != options.profile.config_hash ||
        (!marker.empty() && digest(read_text(root / marker, 1024 * 1024, false)) != marker_hash))
        throw Error("route-revision-conflict", "Canonical policy or configuration changed; no automatic reattachment.");
      if (local_project_lifecycle(workspace) != lifecycle ||
        core::find_existing_adoption_marker(root) != conventional ||
        directory_identity(root) != binding.at("root_identity").get<std::string>() ||
        directory_identity(config.out_dir) != binding.at("index_identity").get<std::string>() ||
        directory_identity(config.state_dir, true) != config.binding.at("state_identity").get<std::string>() ||
        (!attach && !refresh && !grant.active() && store.writer_present()) || store.load() != expected)
        throw Error("binding-changed", "Canonical route changed before returning the result.");
    };
    refresh_stage(request, "final-verification");
    const auto verification_started = core::SteadyClock::now();
    if (local_ready && request.query_session) {
        const auto authority = [config = Config(config), workspace = workspace, binding = Json(binding), root = root, home = home,
                                marker = marker, marker_hash = marker_hash, file = file, config_text = config_text, conventional = conventional,
                                phase = grant.phase(), profile_hash = options.profile.config_hash, lifecycle = lifecycle] {
            if (require_local_project_active(workspace) != lifecycle)
                throw Error("project-detached", "Project lifecycle changed; prepare a new resident reader.");
            StateStore current(workspace, false);
            LocalPublication active(workspace, binding, config);
            if (active.phase() != phase) throw Error("refresh-authority-changed", "Publication authority changed.");
            if (active.approved_profile_hash() != profile_hash)
                throw Error("route-revision-conflict", "Approved profile revision changed.");
            if (active.approved_marker_hash() != marker_hash)
                throw Error("route-revision-conflict", "Approved marker revision changed.");
            if (active.active()) active.verify();
            else if (active.phase() != "read-only" && active.phase() != "revoked")
                throw Error("refresh-grant-incomplete", "Publication authority changed.");
            if (read_text(file, 65536, true) != config_text ||
                !configuration_approved(workspace, binding, digest(config_text)) ||
                core::make_profile(config.profile, config.profile_dir).config_hash != profile_hash ||
                (!marker.empty() && digest(read_text(root / marker, 1024 * 1024, false)) != marker_hash))
                throw Error("route-revision-conflict", "Canonical policy or configuration changed; no automatic reattachment.");
            if (current.load() != std::optional<Json>(binding) ||
                core::find_existing_adoption_marker(root) != conventional ||
                directory_identity(root) != binding.at("root_identity").get<std::string>() ||
                directory_identity(workspace, true) != binding.at("workspace_identity").get<std::string>() ||
                directory_identity(config.out_dir) != binding.at("index_identity").get<std::string>() ||
                directory_identity(config.state_dir, true) != config.binding.at("state_identity").get<std::string>())
                throw Error("binding-changed", "Canonical authority or route changed.");
            guard_ancestor_bindings(root, home);
        };
        *request.query_session = std::make_shared<LocalQuerySession>(config, workspace, workspace / "source-cache", result, authority);
    }
    refresh_stage(request, "final-verification");
    if (options.generation_admission) options.generation_admission->verify_content(options);
    verify_route(loaded);
    result["phase_ms"] = {{"admission", admission_ms}, {"retrieval", retrieval_ms},
        {"final_verification", core::elapsed_ms_since(verification_started)}};
    if (result.dump(2).size() + 1 > 65536) throw Error("output-limit", "Attached route response exceeds the bounded view.");
    guard_ancestor_bindings(root, home);
    require_running();
    if ((attach || refresh) && local_ready) {
        store.recover_pending();
        if (!loaded || binding != *loaded) store.save(binding);
    }
    // Never wait for the writer fence while retaining the shared index lock.
    publication.reset();
    if (attach && request.manage_index && local_ready) {
        if (!marker.empty()) verify_marker_identity(workspace, binding, read_text(root / marker, 1024 * 1024, false), true);
        LocalPublication accepted(workspace, binding, config, true);
        accepted.enable(snapshot, options.profile.config_hash);
        if (profile_text.size() <= 96 * 1024)
            LocalProfileRevision(workspace, binding, accepted).remember(options.profile.config_hash, profile_text);
        result["publication"] = "managed";
        result["refresh_management"] = "local-service";
    }
    const auto qualify = [&] {
        if (!(attach || refresh) || !local_ready || result.at("readiness").at("quality_reuse").at("reused") == true) return;
        const auto read = core::lock_index_for_read(config.out_dir);
        LocalPublication final_grant(workspace, binding, config);
        if (final_grant.active()) final_grant.verify();
        const auto freshness = core::assess_index_freshness(options, true);
        core::require_current_index_or_throw(freshness);
        if (freshness.snapshot_id != snapshot)
            throw Error("query-snapshot-changed", "Publication changed before recording its qualification.");
        verify_route(binding);
        require_running();
        save_qualification(workspace, quality_binding, config, snapshot, result.at("readiness"), quality_key, options, marker_hash);
    };
    if (refresh) {
        auto tick = envelope("watch-tick", local_ready);
        tick.update({{"phase", local_ready ? "current" : "blocked"}, {"rebuilt", published},
            {"refresh_authority", grant.active() ? "local-canonical" : "external-publication-read-only"}, {"workspace_identity", binding.at("workspace_identity")},
            {"index", config.out_dir.string()}, {"snapshot_id", snapshot},
            {"canonical_ready", canonical_ready}, {"observer_sample_available", sampled},
            {"readiness_summary", result.at("readiness").at("summary")},
            {"quality_reuse", result.at("readiness").at("quality_reuse")},
            {"recovered", local_ready && repaired ? "primary-index" :
                local_ready && result.at("readiness").at("quality_reuse").at("cache_status") == "invalid" ? "quality-qualification" : ""},
            {"diagnostic_ids", result.at("readiness").at("diagnostic_ids")},
            {"prepared_new_snapshot", local_ready && loaded->at("prepared_snapshot") != binding.at("prepared_snapshot")},
            {"next_action", result.at("next_action")}});
        tick["refresh_work"] = qualification_metrics.values;
        if (published_qualification && published_qualification->work.is_object())
            tick["refresh_work"].update(published_qualification->work);
        if (!local_ready) tick["error"] = error_json(Error("project-check-failed",
            "Canonical readiness checks failed.", readiness_diagnostic(result.at("readiness"))));
        if (!managed_observation.is_null()) tick["observation"] = managed_observation;
        if (tick.dump(2).size() + 1 > 65536) throw Error("output-limit", "Refresh result exceeds its bounded view.");
        qualify();
        return tick;
    }
    const auto watch_directory = workspace / "watch";
    if (present(watch_directory)) {
        result["watch"] = local_watch_status(watch_directory);
    }
    if (result.dump(2).size() + 1 > 65536) throw Error("output-limit", "Attached route response exceeds the bounded view.");
    qualify();
    return result;
    });
}
} // namespace

Json local_watch_status(const fs::path& directory) {
    StateStore controller(directory, false);
    Json result{{"running", controller.writer_present()}, {"state", nullptr}};
    try { result["state"] = controller.load(); }
    catch (const Error& error) {
        if (error.code != "state-changed") throw;
        // Watch progress is observational; authority is checked separately.
        result["state_error_code"] = error.code;
    }
    return result;
}

static Json local_project_impl(const LocalProjectRequest& input) {
    auto request = input;
    const bool initialize = request.command == "init";
    bool attach = request.command == "attach" || (initialize && request.observer_config.has_value());
    const bool refresh = request.command == "_refresh";
    const bool release = request.command == "refresh-release";
    const bool approve_profile = request.command == "profile-accept";
    const bool approve_marker = request.command == "marker-accept";
    const bool approve_config = request.command == "config-accept";
    const bool detach = request.command == "_detach" || request.command == "_detach-complete";
    const bool writer = initialize || attach || refresh || release || approve_profile || approve_marker || approve_config || detach;
    const bool followup = request.command == "inspect" || request.command == "read";
    if (!writer && request.command != "profile-review" && request.command != "status" && request.command != "query" && !followup)
        throw Error("invalid-arguments", "Supported local commands are init, attach, status, query, inspect and read.");
    if (followup) {
        if (request.snapshot.size() != 24 || !request.snapshot.starts_with("fnv1a64:") ||
            !std::all_of(request.snapshot.begin() + 8, request.snapshot.end(),
                [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            throw Error("invalid-source-snapshot", "Follow-up requires the exact snapshot_id returned by query.");
        if (request.path.empty() == request.record.empty() ||
            (request.command == "inspect" && !request.record.empty()) ||
            request.path.size() > 4096 || request.record.size() > 1024 ||
            request.path.find('\0') != std::string::npos || request.record.find('\0') != std::string::npos)
            throw Error("invalid-arguments", "Select one path, or one record ID for read, within the follow-up limits.");
    }
    if (request.command == "query" && (request.query.empty() || request.query.size() > 4096 ||
        request.query.find('\0') != std::string::npos))
        throw Error("invalid-query", "Query must contain 1..4096 bytes without NUL.");
    const auto root = fs::canonical(request.root);
    if (root == root.root_path()) throw Error("unsafe-root", "A filesystem root cannot be a project.");
    const auto root_identity = directory_identity(root);
    if (!request.maintenance_root_identity.empty() && request.maintenance_root_identity != root_identity)
        throw Error("refresh-root-changed", "The watched project directory was replaced.");
    const auto home = fs::absolute(request.home).lexically_normal();
    if (fs::weakly_canonical(home) != home || overlaps(home, root))
        throw Error("unsafe-home", "Private state must be outside the project, without symlink components.");
    const auto projects = home / "projects";
    const auto workspace = projects / digest(root.string());
    if (!request.maintenance_workspace_identity.empty() &&
        request.maintenance_workspace_identity != directory_identity(workspace, true))
        throw Error("refresh-workspace-changed", "The watched private workspace was replaced.");
    for (const auto& path : {home, projects})
        if (present(path)) (void)directory_identity(path, true);
    if (initialize || attach) check_local_identity(home,root,true);
    guard_ancestor_bindings(root, home);
    std::optional<Json> detected;
    std::unique_ptr<StateStore> binding_store;
    bool attached = false;
    if (present(workspace)) {
        for (const auto& path : {home, projects, workspace}) (void)directory_identity(path, true);
        // Detection and mutation share one lock: pending first publication is
        // not abandoned state while another initializer still owns the writer.
        if (writer) binding_store = std::make_unique<StateStore>(workspace, true);
        StateStore existing(workspace, false);
        detected = binding_store ? binding_store->load() : existing.load();
        guard_unbound_workspace(workspace, detected);
        attached = attached_binding(detected);
    }
    if (initialize && attached) attach = true;
    if (release && !attached)
        throw Error("refresh-grant-required", "Only an attached canonical route has a releasable grant.");
    if ((approve_profile || request.command == "profile-review") && !attached)
        throw Error("refresh-grant-required", "Only an attached canonical route supports profile approval.");
    if (approve_marker && !attached)
        throw Error("refresh-grant-required", "Only an attached canonical route supports marker approval.");
    if (approve_config && !attached)
        throw Error("binding-unavailable", "Only an attached canonical route supports configuration approval.");
    if (attach && request.profile_dir_explicit)
        throw Error("invalid-arguments", "Existing project settings own the profile; --profile-dir cannot override them.");
    if (initialize && request.marker && !attach)
        throw Error("invalid-arguments", "An existing project configuration is required with --marker; pass --observer-config FILE once.");
    if (attach && detected && !attached)
        throw Error("binding-mode-conflict", "Existing local bootstrap binding detected; use shin-ce init, not attach. Automatic conversion is disabled.");
    if (attach && !request.observer_config) {
        if (!detected || !attached) throw Error("invalid-arguments", "First attach requires --observer-config FILE; no configuration is guessed.");
        if (!detected->contains("observer_config") || !detected->at("observer_config").is_string())
            throw Error("binding-invalid", "Attached route configuration is missing from the binding.");
        request.observer_config = detected->at("observer_config").get<std::string>();
    }
    if (initialize || attach) {
        if (detected && !attached) verify_binding(*detected,root,root_identity,workspace);
        guard_project_storage(home,root);
    }
    fs::path storage_target = home;
    std::uintmax_t storage_reservation = 1024ULL * 1024 * 1024;
    if (attach) {
        const auto config_file = fs::absolute(*request.observer_config).lexically_normal();
        const auto config = load_config(config_file);
        require_route_separation(config, config_file, root, home);
        guard_external_storage(home,root,config.out_dir);
        guard_external_storage(home,root,config.state_dir);
        storage_target = config.out_dir;
        storage_reservation = 2 * config.max_snapshot_bytes + 512ULL * 1024 * 1024;
    } else if (!attached) guard_existing_routes(root, home);
    if ((!writer || refresh) && !present(workspace)) {
        auto result = envelope(request.command, false);
        result.update({{"local_ready", false}, {"state", "not-initialized"}, {"next_action", "shin-ce init --offline"}});
        return result;
    }
    std::unique_ptr<LocalStorageReservation> storage_admission;
    if (initialize || attach) storage_admission = std::make_unique<LocalStorageReservation>(home,storage_target,storage_reservation,root);
    for (const auto& path : {home, projects, workspace}) {
        if (writer) ensure_private_directory(path);
        else (void)directory_identity(path, true);
    }
    if (!binding_store) binding_store = std::make_unique<StateStore>(workspace, writer);
    auto& store = *binding_store;
    if (!writer && !attached && store.writer_present()) throw Error("project-busy", "Project initialization is in progress.");
    auto loaded = store.load();
    if (loaded != detected) throw Error("binding-changed", "Local route changed during detection; retry without replacing it.");
    guard_unbound_workspace(workspace, loaded);
    guard_ancestor_bindings(root, home);
    const auto lifecycle = local_project_lifecycle(workspace);
    if (detach) {
        if (!loaded || loaded->value("root", "") != root.string() ||
            loaded->value("root_identity", "") != root_identity ||
            loaded->value("workspace_identity", "") != directory_identity(workspace, true))
            throw Error("binding-identity-changed", "Detach requires the original prepared project binding.");
        if (request.command == "_detach-complete") {
            if (lifecycle.phase != "detaching" && lifecycle.phase != "detached")
                throw Error("lifecycle-invalid", "Detach has not been prepared.");
            if (present(workspace / "watch") && StateStore(workspace / "watch", false).writer_present())
                throw Error("project-busy", "Project controller is still stopping.");
            set_local_project_lifecycle(workspace, *loaded, "detached");
        } else {
            if (lifecycle.phase != "detached") set_local_project_lifecycle(workspace, *loaded, "detaching");
            if (attached_binding(loaded)) (void)revoke_local_publication(workspace, *loaded);
            unenroll_local_project(home, *loaded);
        }
        auto result = envelope("detach", true);
        result.update({{"root", root.string()}, {"state", local_project_lifecycle(workspace).phase},
            {"data_preserved", true}, {"service_enrolled", false}, {"publication", "disabled"},
            {"next_action", "shin-ce init"}});
        return result;
    }
    if (lifecycle.phase != "active") {
        if (!initialize || lifecycle.phase != "detached")
            throw Error("project-detached", "Project is disconnected or still detaching; finish detach before explicit init.");
        if (!loaded || loaded->value("root", "") != root.string() || loaded->value("root_identity", "") != root_identity)
            throw Error("binding-identity-changed", "Reconnect requires the original project identity.");
    }
    if (release) {
        if (!loaded || !attached_binding(loaded)) throw Error("refresh-grant-required", "Only an attached canonical route has a releasable grant.");
        auto result = envelope("refresh-release", true);
        result.update(revoke_local_publication(workspace, *loaded));
        return result;
    }
    if (!loaded && (!writer || refresh)) throw Error("binding-unavailable", "Local project binding is missing.");
    if (attach || attached_binding(loaded)) {
        auto result = attached_project(request, root, home, workspace, store, loaded, lifecycle);
        if (initialize && result.at("ok") == true && lifecycle.phase == "detached") {
            const auto rebound = store.load();
            if (!rebound) throw Error("binding-unavailable", "The reconnected project binding is missing.");
            set_local_project_lifecycle(workspace, *rebound, "active");
        }
        return result;
    }
    if (request.manage_index)
        throw Error("invalid-arguments", "New private projects already refresh automatically; --manage-index is only for an existing canonical route.");
    if (loaded) verify_binding(*loaded, root, root_identity, workspace);
    if (initialize) check_local_identity(home,root);
    if (!loaded && !request.profile_dir) throw Error("profiles-unavailable", "Initial attachment requires bundled profiles.");
    const auto registry = loaded ? fs::path(loaded->at("profile_dir").get<std::string>()) :
                                  fs::canonical(request.profile_dir.value());
    if (overlaps(home, registry)) throw Error("unsafe-home", "Private state must be separate from the profile registry.");
    if (loaded && request.profile_dir_explicit && request.profile_dir && fs::canonical(*request.profile_dir) != registry)
        throw Error("binding-profile-conflict", "An existing project cannot silently switch profile registries.");
    auto policy = core::inspect_profile_policy(root, registry, "generic-source",
        loaded ? std::optional<std::string>(loaded->at("profile_config_hash").get<std::string>()) : std::nullopt);
    if (policy.at("ok") != true) {
        auto result = envelope(request.command, false);
        result.update({{"local_ready", false}, {"state", "invalid-policy"}, {"policy", std::move(policy)}});
        return result;
    }
    if (policy.at("fields").at("bootstrapOnly").at("value") != true ||
        policy.at("rule_sets").at("evalCases") != 0 || policy.at("rule_sets").at("packEvalCases") != 0)
        throw Error("unsupported-local-profile", "Do not bypass configured project quality gates through bootstrap.");
    Json binding = loaded ? *loaded : Json{{"schema_version", "shin.local-project-binding.v1"},
        {"root", root.string()}, {"root_identity", root_identity},
        {"workspace_identity", directory_identity(workspace, true)}, {"profile_dir", registry.string()},
        {"profile_config_hash", policy.at("revision").at("profile_config_hash")},
        {"core_version", SHIN_VERSION}, {"generator_version", core::kGeneratorVersion}, {"prepared_snapshot", ""}};
    if (initialize) {
        store.recover_pending();
        if (!loaded) store.save(binding);
        for (const auto* name : {"index", "observer", "config"}) ensure_private_directory(workspace / name);
        StateStore config_store(workspace / "config", true);
        const auto previous = config_store.load();
        const auto expected = observer_configuration(workspace, root, registry);
        if (previous && *previous != expected)
            throw Error("binding-config-conflict", "Existing Observer configuration differs; no automatic overwrite.");
        config_store.recover_pending();
        if (!previous) config_store.save(expected);
    }
    StateStore config_reader(workspace / "config", false);
    const auto config_document = config_reader.load();
    if (!config_document || *config_document != observer_configuration(workspace, root, registry))
        throw Error("binding-config-conflict", "Bound Observer configuration differs from local offline policy.");
    const auto config = config_from_json(*config_document, workspace / "config/state.json");
    core::RuntimeOptions options;
    options.root = root;
    options.out_root = workspace / "index";
    options.profile_dir = registry;
    options.profile = core::make_profile("generic-source", registry);
    if (!options.profile.bootstrap_only || !options.profile.eval_cases.empty() || !options.profile.pack_eval_cases.empty())
        throw Error("unsupported-local-profile", "This local entry supports bootstrap only; use the canonical route for project quality gates.");
    if (options.profile.config_hash != binding.at("profile_config_hash").get<std::string>())
        throw Error("binding-profile-conflict", "Profile changed during local attachment.");
    const bool engine_changed = binding.at("core_version") != SHIN_VERSION ||
                                binding.at("generator_version") != core::kGeneratorVersion;
    const auto quality_key = qualification_key(binding, options.profile.config_hash);
    const auto readiness_for_generation = [&] {
        refresh_stage(request, "qualification");
        return qualified_readiness(options, {}, config, workspace, binding, quality_key);
    };
    const auto qualify = [&](const Json& readiness) {
        if (readiness.at("ok") == true && readiness.at("quality_reuse").at("reused") == false)
            save_qualification(workspace, binding, config, binding.at("prepared_snapshot"), readiness, quality_key, options);
    };
    if (refresh) {
        if (engine_changed) throw Error("refresh-engine-changed", "Review the installed kit and run init once after an engine upgrade.");
        recover_refresh_candidate(workspace);
        if (shin::generated_io::publication_in_progress(options.out_root)) {
            shin::generated_io::IndexLock recovery_writer(options.out_root, shin::generated_io::LockMode::exclusive,
                                                 shin::generated_io::LockTarget::writer);
            shin::generated_io::recover_snapshot(options.out_root, core::kGeneratedDir);
        }
        refresh_stage(request, "observation");
        auto observation = sample_worktree(config, root_identity);
        std::optional<SnapshotRepairBase> repair;
        if (observation.at("status") == "error" && observation.at("reason_code").is_string() &&
            repairable_snapshot_error(observation.at("reason_code"))) {
            const auto read = core::lock_index_for_read(config.out_dir);
            repair = inspect_snapshot_repair(config, binding.at("prepared_snapshot"));
            observation = sample_worktree(config, root_identity, true);
        }
        auto result = envelope("watch-tick", true);
        result.update({{"index", options.out_root.string()}, {"observation", observation},
            {"rebuilt", false}, {"phase", "current"}, {"refresh_authority", "private-bootstrap"},
            {"workspace_identity", binding.at("workspace_identity")}, {"recovery_pending", repair.has_value()}});
        if (observation.at("status") != "current" && observation.at("status") != "stale" &&
            !(repair && observation.at("status") == "unpublished")) {
            result.update({{"ok", false}, {"phase", "blocked"}});
            result["error"] = source_observation_error(observation);
            return result;
        }
        const auto authority = [&] {
            if (request.maintenance_stop && request.maintenance_stop())
                throw Error("refresh-stopped", "Local refresh was stopped before publication.");
            guard_existing_routes(root, home);
            if (directory_identity(root) != root_identity || store.load() != loaded ||
                config_reader.load() != config_document ||
                core::make_profile("generic-source", registry).config_hash != options.profile.config_hash)
                throw Error("refresh-authority-changed", "Project, profile or configuration changed during refresh.");
        };
        if (observation.at("snapshot_current") == true &&
            binding.at("prepared_snapshot") == observation.at("snapshot_id")) {
            const auto read = core::lock_index_for_read(options.out_root);
            const auto readiness = readiness_for_generation();
            if (readiness.at("ok") != true)
                throw Error("refresh-not-current", "Current sources no longer qualify their generated output.");
            refresh_stage(request, "final-verification");
            options.generation_admission->verify_content(options);
            authority();
            qualify(readiness);
            return result;
        }
        if (request.maintenance_worktree != observation.at("worktree_id").get<std::string>()) {
            result["phase"] = "coalescing";
            return result;
        }
        store.recover_pending();
        authority();
        if (observation.at("snapshot_current") != true) {
            publish_refreshed(options, config, workspace, request.maintenance_worktree, authority, nullptr, repair, {}, request.maintenance_progress);
            result["rebuilt"] = true;
        }
        const auto observed = run_observer(config, 1, [] { return false; });
        const auto read = core::lock_index_for_read(options.out_root);
        const auto readiness = readiness_for_generation();
        if (readiness.at("ok") != true)
            throw Error("refresh-not-current", "Source changed during refresh; no current context is declared.");
        refresh_stage(request, "source-cache");
        prepare_with_source_fence(authority, [&] {
            const auto current = sample_worktree(config, root_identity);
            if (current.at("status") == "stale" && current.at("worktree_id") != observed.at("observation").at("worktree_id"))
                throw Error("refresh-superseded", "Source changed during search preparation.");
        }, [&] {
            core::SourceDiscoveryIndex prepared(options, config.state_dir / "source-cache");
        });
        authority();
        const auto freshness = core::assess_index_freshness(options, true);
        core::require_current_index_or_throw(freshness);
        binding["prepared_snapshot"] = freshness.snapshot_id;
        store.save(binding);
        qualify(readiness);
        result["phase"] = "current";
        result["recovered"] = repair ? "primary-index" : "";
        result["recovery_pending"] = false;
        result["snapshot_id"] = freshness.snapshot_id;
        result["observation"] = observed.at("observation");
        return result;
    }
    // Initial bootstrap may not have an index/lock yet; established readers
    // retain the same publication fence as attached projects.
    auto publication = core::generated_index_available(options) ? core::lock_index_for_read(options.out_root) : nullptr;
    auto readiness = readiness_for_generation();
    bool rebuilt = false;
    if (initialize) {
        if (engine_changed || readiness.at("ok") != true) {
            publication.reset();
            const auto build = core_result([&] { return core::run_build(options); });
            if (build.at("ok") != true) throw Error("build-failed", "Native project publication failed.");
            rebuilt = true;
        }
        if (!publication) publication = core::lock_index_for_read(options.out_root);
        guard_existing_routes(root, home);
        if (directory_identity(root) != root_identity)
            throw Error("binding-identity-changed", "Project identity or adoption changed during init.");
        (void)run_observer(config, 1, [] { return false; });
        readiness = readiness_for_generation();
        if (readiness.at("ok") == true) {
            core::SourceDiscoveryIndex prepared(options, config.state_dir / "source-cache");
            binding["core_version"] = SHIN_VERSION;
            binding["generator_version"] = core::kGeneratorVersion;
            const auto freshness = core::assess_index_freshness(options, true);
            core::require_current_index_or_throw(freshness);
            if (readiness.at("freshness").at("snapshot_id") != freshness.snapshot_id)
                throw Error("query-snapshot-changed", "Publication changed during search preparation.");
            binding["prepared_snapshot"] = freshness.snapshot_id;
            if (store.load() != std::optional<Json>(binding)) store.save(binding);
        }
    }
    const auto observer = observer_status(config);
    const bool sampled = !observer.at("state").is_null();
    if (sampled && observer.at("state").at("root_identity") != root_identity)
        throw Error("binding-identity-changed", "Observer belongs to a different project directory.");
    const bool prepared = readiness.contains("freshness") &&
        binding.at("prepared_snapshot") == readiness.at("freshness").at("snapshot_id");
    const bool ready = readiness.at("ok") == true && sampled && prepared && (!engine_changed || initialize);
    auto result = envelope(request.command, ready);
    result.update({{"local_ready", ready}, {"state", ready ? "local-ready" : "needs-init"},
        {"canonical_ready", false}, {"project_quality", "not-configured"},
        {"readiness_scope", "local-bootstrap-not-project-quality-qualification"},
        {"project_id", config.binding.at("project_id")}, {"workspace_id", config.binding.at("workspace_id")},
        {"root", root.string()}, {"index", options.out_root.string()},
        {"profile_config_hash", binding.at("profile_config_hash")}, {"rebuilt", rebuilt},
        {"readiness", readiness}, {"synchronization", "disabled"},
        {"next_action", ready ? "shin-ce query" : "shin-ce init --offline"}});
    const auto watch_directory = workspace / "watch";
    if (present(watch_directory)) {
        result["watch"] = local_watch_status(watch_directory);
        if (!ready && result.at("watch").at("running") == true) result["next_action"] = "wait-for-local-refresh";
    }
    if (ready && request.command == "query") {
        add_query_context(result, options, config, request.query);
    }
    if (ready && followup) add_followup_context(result, config, request, options, binding.at("prepared_snapshot"));
    if (ready && request.query_session) {
        // Resident readers fence the published index and compare committed
        // authority before/after. An unchanged maintenance writer is not a
        // source change; its lock alone must not evict this generation.
        const auto authority = [config = Config(config), workspace = workspace, binding = Json(binding), root = root,
                                home = home, config_document = config_document,
                                output_identity = directory_identity(config.out_dir), lifecycle = lifecycle] {
            if (require_local_project_active(workspace) != lifecycle)
                throw Error("project-detached", "Project lifecycle changed; prepare a new resident reader.");
            StateStore current(workspace, false);
            StateStore settings(workspace / "config", false);
            guard_existing_routes(root, home);
            if (current.load() != std::optional<Json>(binding) || settings.load() != config_document ||
                core::make_profile(config.profile, config.profile_dir).config_hash != binding.at("profile_config_hash").get<std::string>() ||
                directory_identity(root) != binding.at("root_identity").get<std::string>() ||
                directory_identity(workspace, true) != binding.at("workspace_identity").get<std::string>() ||
                directory_identity(config.out_dir) != output_identity ||
                directory_identity(config.state_dir, true) != config.binding.at("state_identity").get<std::string>())
                throw Error("binding-changed", "Private authority or route changed.");
        };
        *request.query_session = std::make_shared<LocalQuerySession>(config, workspace, config.state_dir / "source-cache", result, authority);
    }
    // Native-only observation also lets reader races be tested at this boundary.
    refresh_progress(request.maintenance_progress, "final-verification");
    if (options.generation_admission) options.generation_admission->verify_content(options);
    guard_existing_routes(root, home);
    if (local_project_lifecycle(workspace) != lifecycle || directory_identity(root) != root_identity ||
        store.load() != std::optional<Json>(binding) || config_reader.load() != config_document)
        throw Error("binding-changed", "Project binding changed before returning the result.");
    if (!initialize && store.writer_present())
        throw Error("project-busy", "Project maintenance is in progress; retry the request.");
    if (result.dump(2).size() + 1 > 65536) throw Error("output-limit", "Response exceeds the bounded local view; narrow the query.");
    if (initialize && ready) qualify(readiness);
    if (initialize && ready && lifecycle.phase == "detached")
        set_local_project_lifecycle(workspace, binding, "active");
    return result;
}

Json local_project(const LocalProjectRequest& request) {
    refresh_stage(request, "admission");
    const auto before = core::generation_execution_count();
    std::shared_ptr<LocalQuerySession> prepared;
    auto admitted = request;
    if (request.query_session) {
        request.query_session->reset();
        admitted.query_session = &prepared;
    }
    auto result = local_project_impl(admitted);
    if ((request.command == "init" || request.command == "attach") && result.at("ok") == true)
        result["identity"] = local_identity(request,true);
    result["work_performed"] = {{"full_generations", core::generation_execution_count() - before}};
    if (result.dump(2).size() + 1 > 65536)
        throw Error("output-limit", "Response exceeds the bounded local view.");
    if (request.query_session && result.at("ok") == true) *request.query_session = std::move(prepared);
    return result;
}

namespace {
class LocalWatch {
public:
    LocalWatch(LocalProjectRequest input, const std::function<bool()>& stopped, LocalRefresh execute)
        : request(std::move(input)), refresh(std::move(execute)) {
        request.command = "_refresh";
        request.home = fs::absolute(request.home).lexically_normal();
        const auto identity = directory_identity(request.root);
        if (!request.maintenance_root_identity.empty() && request.maintenance_root_identity != identity)
            throw Error("registry-binding-changed", "Enrolled source directory identity changed.");
        request.maintenance_root_identity = identity;
        request.maintenance_stop = stopped;
        const auto workspace = request.home / "projects" / digest(request.root.string());
        const auto lifecycle = require_local_project_active(workspace);
        const auto admission = refresh ? refresh(request) : local_project(request);
        if (!admission.contains("refresh_authority") ||
            (admission.at("refresh_authority") != "private-bootstrap" &&
             admission.at("refresh_authority") != "external-publication-read-only" &&
             admission.at("refresh_authority") != "local-canonical"))
            throw Error("refresh-binding-required", "Initialize every local project before watching it.");
        request.maintenance_workspace_identity = admission.at("workspace_identity").get<std::string>();
        StateStore binding_writer(workspace, true, request.maintenance_workspace_identity, false);
        if (require_local_project_active(workspace) != lifecycle)
            throw Error("project-detached", "Project lifecycle changed during watch admission.");
        if (directory_identity(workspace, true) != request.maintenance_workspace_identity)
            throw Error("refresh-workspace-changed", "The private workspace changed during controller admission.");
        const auto directory = workspace / "watch";
        ensure_private_directory(directory);
        controller = std::make_unique<StateStore>(directory, true);
        std::optional<Json> previous;
        bool recovered = false;
        try { previous = controller->load(); }
        catch (const Error& error) {
            if (!damaged_derived_content(error)) throw;
            recovered = true;
        }
        if (previous) {
            if (previous->is_object() && previous->contains("root") && previous->at("root").is_string() &&
                previous->at("root") != request.root.string())
                throw Error("watch-state-invalid", "Controller metadata belongs to a different project.");
            if (!previous->is_object() ||
                previous->value("schema_version", Json(nullptr)) != "shin.local-watch-state.v1" ||
                previous->value("root", Json(nullptr)) != request.root.string() ||
                !previous->contains("phase") || !previous->at("phase").is_string()) {
                previous.reset();
                recovered = true;
            }
        }
        controller->recover_pending();
        const auto admission_recovery = admission.value("recovered", "");
        const bool admission_recovered = admission_recovery == "quality-qualification" || admission_recovery == "primary-index";
        // Notifications only accelerate the next full check. A bounded periodic
        // check still covers external policy/authority files and missed events.
        changes = make_monitor();
        epoch = changes->sequence();
        state = {{"schema_version", "shin.local-watch-state.v1"}, {"root", request.root.string()},
            {"phase", "starting"}, {"ticks", 0}, {"publications", 0}, {"preparations", 0}, {"failures", 0},
            {"superseded_refreshes", 0}, {"suspended_refreshes", 0},
            {"recoveries", unsigned(recovered) + unsigned(admission_recovered)},
            {"last_recovery", recovered ? "watch-state" : admission_recovered ? admission_recovery : ""},
            {"previous_run_interrupted", previous && previous->value("phase", "") != "stopped"},
            {"last_result", nullptr}, {"refresh_authority", admission.at("refresh_authority")},
            {"observed_at_ms", now_ms()}};
    }

    bool pending(std::uint64_t samples) const {
        return controller && (samples == 0 || state.at("ticks").get<std::uint64_t>() < samples);
    }

    bool ready() {
        const auto now = std::chrono::steady_clock::now();
        if (changes->sequence() != epoch || !changes->healthy()) {
            epoch = changes->sequence();
            idle_checks = 0;
            // Loss of native notifications restores the regular scan cadence,
            // never a 20ms scan storm or indefinite suppression of refresh.
            if (!failures && !storage_retry)
                due = std::min(due, last_completed + std::chrono::seconds(1));
        }
        return now >= due;
    }

    void wake_for_reader() {
        // A lost reader is not a source notification. Wake an idle controller,
        // retaining its normal coalescing floor and all failure/storage backoff.
        if (controller && !failures && !storage_retry)
            due = std::min(due, last_completed + std::chrono::seconds(1));
    }

    void sample(const std::function<void()>& checking = {}) {
        const auto start = std::chrono::steady_clock::now();
        if (!changes->healthy()) last_monitor_error = changes->failure_reason();
        if (!changes->healthy() && start >= next_monitor_retry) {
            // Recreate only the observation stream, never authority or indexes.
            // The new stream must exist before the full reconciliation below.
            next_monitor_retry = start + std::chrono::seconds(30);
            try { if (directory_identity(request.root) == request.maintenance_root_identity) {
                auto replacement = make_monitor();
                if (replacement->healthy()) {
                    changes = std::move(replacement);
                    epoch = changes->sequence();
                    idle_checks = 0;
                    monitor_requalification = true;
                    ++monitor_restarts;
                }
            } } catch (const std::exception&) {
                if (last_monitor_error.empty()) last_monitor_error = "monitor-restart-failed";
                // The normal refresh below reports identity/source errors. A
                // failed observation-stream retry must not retire its controller.
            }
        }
        tick_before = state.at("ticks").get<std::uint64_t>();
        if (storage_retry) controller->recover_pending();
        state["phase"] = "checking";
        controller->save(state);
        storage_retry = false;
        state["state_persisted"] = true;
        if (checking) checking();
        try {
            auto tick = refresh ? refresh(request) : local_project(request);
            if (!tick.contains("refresh_authority"))
                throw Error("refresh-policy-invalid", "Bound local policy no longer permits refresh.");
            if (failed_source_observation_tick(tick)) {
                request.maintenance_worktree.clear();
                state["refresh_authority"] = tick.at("refresh_authority");
                record_refresh_error(source_observation_error(tick.at("observation"), tick.value("error", Json())));
                tick = classified_source_observation_tick(std::move(tick), state.at("last_result"),
                                                          state.at("phase").get<std::string>());
                state["last_result"] = std::move(tick);
            } else {
                request.maintenance_worktree.clear();
                if (tick.contains("observation") && tick.at("observation").is_object() &&
                    tick.at("observation").contains("worktree_id") && tick.at("observation").at("worktree_id").is_string())
                    request.maintenance_worktree = tick.at("observation").at("worktree_id").get<std::string>();
                const bool ok = tick.at("ok").get<bool>();
                const auto recovery = tick.value("recovered", "");
                if (ok && (recovery == "quality-qualification" || recovery == "primary-index")) {
                    state["recoveries"] = state.at("recoveries").get<std::uint64_t>() + 1;
                    state["last_recovery"] = recovery;
                }
                failures = ok ? 0 : std::min(5U, failures + 1);
                state["phase"] = tick.at("phase");
                state["refresh_authority"] = tick.at("refresh_authority");
                state["publications"] = state.at("publications").get<std::uint64_t>() + (tick.at("rebuilt") == true ? 1 : 0);
                state["preparations"] = state.at("preparations").get<std::uint64_t>() +
                    (tick.value("prepared_new_snapshot", false) ? 1 : 0);
                state["last_result"] = tick;
            }
        } catch (const Error& error) {
            record_refresh_error(error_json(error));
        } catch (const core::CliError& error) {
            Json failure{{"code", error.code()}, {"message", error.what()}};
            if (!error.diagnostic().is_null()) failure["diagnostic"] = error.diagnostic();
            record_refresh_error(failure);
        } catch (const std::exception&) {
            failures = std::min(5U, failures + 1);
            state["phase"] = "blocked";
            state["last_result"] = {{"ok", false}, {"error", {{"code", "refresh-failed"},
                {"message", "Local refresh failed; inspect the owned index and available resources."}}}};
        }
        if (failures) request.maintenance_worktree.clear();
        state["ticks"] = state.at("ticks").get<std::uint64_t>() + 1;
        state["failures"] = state.at("failures").get<std::uint64_t>() + (failures ? 1 : 0);
        state["observed_at_ms"] = now_ms();
        const auto& result = state.at("last_result");
        const bool monitor_stable = changes->healthy() && changes->sequence() == epoch;
        if (monitor_requalification && (state.at("phase") == "current" || state.at("phase") == "partial") && result.value("ok", false) && monitor_stable) {
            monitor_requalification = false;
            ++monitor_recoveries;
        }
        const bool idle = !monitor_requalification && (state.at("phase") == "current" || state.at("phase") == "partial") && result.value("ok", false) &&
            !result.value("rebuilt", false) && !result.value("recovery_pending", false) &&
            !result.contains("error") && changes->healthy() && changes->sequence() == epoch;
        idle_checks = idle ? std::min(4U, idle_checks + 1) : 0;
        state["retry_after_ms"] = failures ? std::min(30000U, 1000U << failures) :
            std::min(15000U, 1000U << idle_checks);
        state["change_monitor_healthy"] = changes->healthy() && !monitor_requalification;
        state["change_monitor_restarts"] = monitor_restarts;
        state["change_monitor_recoveries"] = monitor_recoveries;
        state["last_change_monitor_error"] = last_monitor_error;
        state["idle_checks"] = idle_checks;
        state["idle_max_delay_ms"] = 15000;
        state["last_tick_duration_ms"] = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        controller->save(state);
        last_completed = std::chrono::steady_clock::now();
        due = last_completed + std::chrono::milliseconds(state.at("retry_after_ms").get<unsigned>());
    }

    void stop() {
        if (!controller) return;
        state["phase"] = "stopped";
        controller->save(state);
        controller.reset();
    }

    void storage_failed(const Error* error = nullptr) {
        // Keep the same verified writer for known OS resource failures. Unsafe
        // or replaced storage is never reopened or silently made trusted.
        const bool retry = error && (error->code == "storage-full" || error->code == "storage-read-only" || error->code == "access-denied");
        state["phase"] = "blocked";
        state["state_persisted"] = false;
        state["last_result"] = {{"ok", false}, {"error", {{"code", retry ? error->code : "watch-state-unavailable"},
            {"message", "Controller state could not be persisted; review this project's private storage."}}}};
        if (retry) {
            storage_retry = true;
            if (state.at("ticks") == tick_before) state["ticks"] = tick_before + 1;
            failures = std::min(5U, failures + 1);
            state["failures"] = state.at("failures").get<std::uint64_t>() + 1;
            state["retry_after_ms"] = std::min(30000U, 1000U << failures);
            due = std::chrono::steady_clock::now() + std::chrono::milliseconds(state.at("retry_after_ms").get<unsigned>());
        } else controller.reset();
    }

    Json state;
    std::chrono::steady_clock::time_point due{};
private:
    std::unique_ptr<LocalChanges> make_monitor() const {
        return std::make_unique<LocalChanges>(std::vector<fs::path>{request.root},
            [root = request.root](const fs::path& path, bool directory) {
                const auto relative = path.lexically_relative(root);
                if (core::shin_internal_control_path(relative.generic_string())) return false;
                // Scheduling hints only: excluded dependency/build directories
                // remain covered by the periodic full reconciliation and the
                // reader's independent, profile-aware source verification.
                for (auto it = relative.begin(); it != relative.end(); ++it) {
                    if (!directory && std::next(it) == relative.end()) break;
                    if (core::should_skip_source_fact_dir(it->string())) return false;
                }
                return true;
            });
    }

    void record_refresh_error(const Json& error) {
        const auto code = error.at("code").get<std::string>();
        // Do not infer transience from a generic "changed" suffix: identity,
        // authority and publication changes require their existing hard refusal.
        if (code == "file-changed-during-verification" || code == "refresh-superseded" ||
            code == "refresh-suspended") {
            failures = 0;
            request.maintenance_worktree.clear();
            state["phase"] = "coalescing";
            const auto counter = code == "refresh-suspended" ? "suspended_refreshes" : "superseded_refreshes";
            state[counter] = state.at(counter).get<std::uint64_t>() + 1;
            state["last_result"] = {{"ok", false}, {"phase", "coalescing"}, {"retry_reason", code}};
            if (error.contains("diagnostic") && !error.at("diagnostic").is_null())
                state["last_result"]["diagnostic"] = error.at("diagnostic");
        } else {
            failures = std::min(5U, failures + 1);
            state["phase"] = "blocked";
            state["last_result"] = {{"ok", false}, {"error", error}};
        }
    }

    LocalProjectRequest request;
    LocalRefresh refresh;
    std::unique_ptr<StateStore> controller;
    std::unique_ptr<LocalChanges> changes;
    std::uint64_t epoch = 0;
    unsigned failures = 0, idle_checks = 0;
    std::uint64_t monitor_restarts = 0, monitor_recoveries = 0;
    bool monitor_requalification = false;
    std::string last_monitor_error;
    std::chrono::steady_clock::time_point next_monitor_retry{};
    std::chrono::steady_clock::time_point last_completed{};
    bool storage_retry = false;
    std::uint64_t tick_before = 0;
};
} // namespace

Json watch_local_projects(const LocalProjectRequest& input, const std::vector<fs::path>& roots,
                          std::uint64_t samples, const std::function<bool()>& stopped,
                          const std::function<void()>& service_poll, const LocalEnrollmentHooks& enrollment,
                          const std::function<void(const fs::path&, const Json&)>& report,
                          const LocalRefresh& refresh, const LocalReaderDemand& reader_demand) {
    if ((roots.empty() && !enrollment.read) || roots.size() > 16 || samples > 3600)
        throw Error("invalid-arguments", "Watch permits 1..16 projects and at most 3600 samples per project.");
    std::vector<fs::path> canonical;
    for (const auto& root : roots) {
        const auto path = fs::canonical(root);
        for (const auto& other : canonical)
            if (overlaps(path, other))
                throw Error("watch-root-conflict", "Watched roots must be distinct and non-overlapping, including aliases.");
        canonical.push_back(path);
    }
    std::vector<std::unique_ptr<LocalWatch>> watches;
    // Admit the whole explicit set before publishing any changed source.
    for (const auto& root : canonical) {
        if (stopped()) break;
        auto request = input;
        request.root = root;
        request.maintenance_worktree.clear();
        try {
            watches.push_back(std::make_unique<LocalWatch>(std::move(request), stopped, refresh));
        } catch (const Error& error) {
            // Cancellation during admission still needs normal watch shutdown
            // and delivery of the service's accepted stop acknowledgement.
            if (stopped() && (error.code == "refresh-stopped" || error.code == "service-request-cancelled")) break;
            throw;
        }
    }
    std::map<fs::path, std::pair<std::string, std::string>> admitted;
    auto next_enrollment = std::chrono::steady_clock::time_point{};
    std::size_t next_watch = 0;
    bool demand_served = false;
    while (!stopped()) {
        for (auto it = watches.begin(); it != watches.end();) {
            const fs::path root = (*it)->state.at("root").get<std::string>();
            const auto failed = [&](const Error* error) {
                (*it)->storage_failed(error);
                auto request = input;
                request.root = root;
                try {
                    if (enrollment.report) enrollment.report(request, error ? error->code : "lifecycle-unavailable");
                } catch (const std::exception&) {
                    // Keep the recorded storage failure without terminating other roots.
                    return;
                }
            };
            try {
                if (local_project_lifecycle(input.home / "projects" / digest(root.string())).phase == "active") { ++it; continue; }
                (*it)->stop();
                if (enrollment.removed) enrollment.removed(root);
            } catch (const Error& error) {
                failed(&error);
            } catch (const std::exception&) { failed(nullptr); }
            admitted.erase(root);
            std::erase(canonical, root);
            it = watches.erase(it);
        }
        if (enrollment.read && std::chrono::steady_clock::now() >= next_enrollment) {
            try {
                const auto requested = enrollment.read();
                if (requested.size() > 16) throw Error("registry-invalid", "Too many project enrollments.");
                for (const auto& [root, identities] : admitted) {
                    (void)identities;
                    if (std::none_of(requested.begin(), requested.end(), [&](const auto& request) { return request.root == root; }))
                        throw Error("registry-removal-unsupported", "Live removal needs explicit lifecycle support; existing bindings stay pinned.");
                }
                if (enrollment.report) enrollment.report({}, "");
                for (auto request : requested) {
                    if (stopped()) break;
                    try {
                        const auto identities = std::pair{request.maintenance_root_identity, request.maintenance_workspace_identity};
                        if (const auto found = admitted.find(request.root); found != admitted.end()) {
                            if (found->second != identities) throw Error("registry-binding-changed", "An admitted identity cannot be replaced.");
                            if (enrollment.report) enrollment.report(request, "");
                            continue;
                        }
                        if (watches.size() == 16 || fs::canonical(request.root) != request.root)
                            throw Error("registry-root-conflict", "Enrolled root is no longer canonical or capacity is full.");
                        for (const auto& root : canonical)
                            if (overlaps(root, request.root)) throw Error("registry-root-conflict", "Enrolled root overlaps an active project.");
                        request.maintenance_worktree.clear();
                        if (enrollment.preparing) enrollment.preparing(request.root, true);
                        auto watch = std::make_unique<LocalWatch>(request, stopped, refresh);
                        canonical.push_back(request.root);
                        admitted.emplace(request.root, identities);
                        watches.push_back(std::move(watch));
                        if (enrollment.report) enrollment.report(request, "");
                    } catch (const Error& error) {
                        if (enrollment.report) enrollment.report(request, error.code);
                    } catch (const std::exception&) {
                        if (enrollment.report) enrollment.report(request, "enrollment-unavailable");
                    }
                    if (enrollment.preparing) enrollment.preparing(request.root, false);
                }
            } catch (const Error& error) {
                if (enrollment.report) enrollment.report({}, error.code);
            } catch (const std::exception&) {
                if (enrollment.report) enrollment.report({}, "registry-unavailable");
            }
            next_enrollment = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        if (service_poll) service_poll();
        bool pending = static_cast<bool>(enrollment.read);
        std::vector<LocalWatch*> due;
        for (std::size_t offset = 0; offset < watches.size(); ++offset) {
            auto& watch = watches[(next_watch + offset) % watches.size()];
            if (watch->pending(samples)) {
                pending = true;
                if (watch->ready()) due.push_back(watch.get());
            }
        }
        LocalWatch* selected = nullptr;
        const auto requested = reader_demand ? reader_demand() : std::vector<fs::path>{};
        for (const auto& root : requested) {
            const auto watch = std::find_if(watches.begin(), watches.end(), [&](const auto& entry) {
                return entry->state.at("root") == root.string();
            });
            if (watch == watches.end() || !(*watch)->pending(samples)) continue;
            (*watch)->wake_for_reader();
            if (!selected && !demand_served && (*watch)->ready()) selected = watch->get();
        }
        const bool prioritized = selected != nullptr;
        if (!selected && !due.empty()) selected = due.front();
        if (selected && !stopped()) {
            // One completion returns to polling/selection so a newly waiting
            // reader need not sit behind an entire round of unrelated refreshes.
            // Alternate priority with due round-robin work to preserve fairness.
            demand_served = prioritized;
            const auto position = std::find_if(watches.begin(), watches.end(), [&](const auto& entry) {
                return entry.get() == selected;
            });
            next_watch = (static_cast<std::size_t>(position - watches.begin()) + 1) % watches.size();
            const auto report_state = [&] {
                if (report) report(fs::path(selected->state.at("root").get<std::string>()), selected->state);
            };
            try { selected->sample([&] { report_state(); if (service_poll) service_poll(); }); }
            catch (const Error& error) { selected->storage_failed(&error); }
            catch (const std::exception&) { selected->storage_failed(); }
            report_state();
        }
        if (!pending) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    for (auto& watch : watches) {
        try { watch->stop(); }
        catch (const std::exception&) { watch->storage_failed(); }
    }
    auto result = envelope("watch", true);
    if (watches.size() == 1) result["watch"] = watches.front()->state;
    else {
        result["scheduler"] = {{"policy", "serial-demand-and-due-round-robin-v2"}, {"max_active_refreshes", 1},
            {"project_count", watches.size()}, {"project_limit", 16}, {"samples_per_project", samples},
            {"query_transport", "independent-local-cli"}};
        result["projects"] = Json::array();
        for (const auto& watch : watches) result["projects"].push_back(watch->state);
    }
    if (result.dump(2).size() + 1 > 65536)
        throw Error("output-limit", "Controller summary exceeds the local response limit; inspect individual project status.");
    return result;
}

Json watch_local_project(const LocalProjectRequest& input, std::uint64_t samples,
                         const std::function<bool()>& stopped) {
    return watch_local_projects(input, {input.root}, samples, stopped);
}
} // namespace shin::observer
