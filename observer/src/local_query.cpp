#include "shin/observer/local_query.h"
#include "local_sessions.h"
#include "shin/observer/local_changes.h"
#include "context_internal.h"
#include "generated_io.h"
#include "generated_snapshot.h"
#include "snapshot_verification.h"
#include "source_discovery.h"
#include "source_read.h"

namespace shin::observer {
Json source_coverage_summary(const Json& manifest) {
    Json files = Json::array();
    std::set<std::string> paths;
    for (const auto* scope : {"source_fact_sources", "sources"}) {
        for (const auto& source : manifest.value(scope, Json::array())) {
            if (source.value("quarantine_reason", std::string{}).empty() ||
                !paths.insert(source.at("path").get<std::string>()).second) continue;
            if (files.size() < 8) {
                files.push_back({{"path", source.at("path")}, {"reason", "secret-like-source-content"}});
                if (files.dump().size() > 4096) files.erase(files.size() - 1);
            }
        }
    }
    if (paths.empty()) return nullptr;
    const auto omitted = paths.size() - files.size();
    return {{"state", "partial"}, {"quarantined_files", paths.size()}, {"files", std::move(files)},
        {"omitted_files", omitted},
        {"notice", "Some files are quarantined after secret detection. Safe project context remains available. Report partial coverage to the user."}};
}
namespace {
namespace core = shin::context;
constexpr std::uintmax_t selected_bytes_limit = 16U * 1024U * 1024U;
constexpr std::size_t selected_files_limit = 64;

bool below(const fs::path& path, const fs::path& parent) {
    const auto [a, b] = std::mismatch(path.begin(), path.end(), parent.begin(), parent.end());
    (void)a;
    return b == parent.end();
}

// The watcher may over-invalidate; it must never miss a possible new input.
bool relevant_source(const core::RuntimeOptions& options, const fs::path& path, bool directory) {
    if (!below(path, options.root)) return false;
    const auto relative = path.lexically_relative(options.root).generic_string();
    if (relative == ".") return directory;
    if (below(path, options.out_root / core::kGeneratedDir) || below(path, options.root / core::kGeneratedDir) ||
        core::shin_internal_control_path(relative)) return false;
    const auto& profile = options.profile;
    for (const auto* files : {&profile.root_files, &profile.optional_root_files})
        for (const auto& file : *files)
            if (relative == file || (directory && below(options.root / file, path))) return true;
    if (below(path, options.root / "docs")) {
        if (directory) return true;
        if (core::document_path_allowed(profile, relative) && core::extension_allowed(profile.source_extensions, relative)) return true;
    }
    for (const auto& source : profile.source_fact_roots) {
        auto base = (options.root / source).lexically_normal();
        if (base.filename().empty()) base = base.parent_path();
        if (directory && below(base, path)) return true;
        if (!below(path, base)) continue;
        // Match the existing source enumerator's generated/dependency pruning.
        const auto inside = path.lexically_relative(base);
        auto components = inside.begin();
        bool pruned = false;
        for (; components != inside.end(); ++components) {
            if (!directory && std::next(components) == inside.end()) break;
            if (core::should_skip_source_fact_dir(components->string())) { pruned = true; break; }
        }
        if (pruned) continue;
        if (directory || core::extension_allowed(profile.source_fact_extensions, relative)) return true;
        if (std::find(profile.source_fact_extractors.begin(), profile.source_fact_extractors.end(), "retool") !=
            profile.source_fact_extractors.end()) return true;
    }
    return false;
}

void collect_paths(const Json& value, const std::map<std::string, Json>& sources, std::set<std::string>& selected) {
    if (value.is_array()) {
        for (const auto& entry : value) collect_paths(entry, sources, selected);
    } else if (value.is_object()) {
        for (const auto& [key, entry] : value.items()) {
            if ((key == "path" || key == "source_path" || key == "target_path") && entry.is_string() &&
                sources.contains(entry.get_ref<const std::string&>())) selected.insert(entry.get<std::string>());
            else if (entry.is_structured()) collect_paths(entry, sources, selected);
        }
    }
}
}

struct LocalQuerySession::Impl {
    Config config;
    fs::path workspace, cache;
    core::RuntimeOptions options;
    Json status;
    bool partial = false;
    std::function<void()> verify_authority;
    std::unique_ptr<LocalChanges> changes;
    std::shared_ptr<const core::GeneratedSnapshot> snapshot;
    std::unique_ptr<core::SourceDiscoveryIndex> discovery;
    std::map<std::string, core::FileMetadata> artifacts;
    std::map<fs::path, core::FileMetadata> cache_files;
    std::map<std::string, Json> sources;
    std::map<std::string, bool> links;
    std::uint64_t epoch = 0;
    double prepared_ms = 0;
    std::uint64_t requests = 0;
    std::int64_t prepared_at_ms = 0;

    Impl(const Config& input, fs::path project_workspace, fs::path cache_directory, Json ready, std::function<void()> authority, const core::Profile* approved_profile)
        : config(input), workspace(std::move(project_workspace)), cache(std::move(cache_directory)),
          status(std::move(ready)), verify_authority(std::move(authority)) {
        const auto started = core::SteadyClock::now();
        options.root = config.root;
        options.out_root = config.out_dir;
        options.profile_dir = config.profile_dir;
        partial = approved_profile != nullptr;
        options.profile = partial ? *approved_profile : core::make_profile(config.profile, config.profile_dir);
        changes = std::make_unique<LocalChanges>(std::vector<fs::path>{config.root},
            [this](const fs::path& path, bool directory) { return relevant_source(options, path, directory); });
        changes->flush();
        if (!changes->healthy()) throw Error("local-watcher-unavailable", "Reactive source observation could not start.");
        epoch = changes->sequence();
        snapshot = core::GeneratedSnapshot::load(options, {config.max_snapshot_bytes, config.max_file_bytes});
        options.generated_snapshot = snapshot;
        const auto coverage = source_coverage_summary(snapshot->manifest());
        if (!coverage.is_null() && !partial) status["source_coverage"] = coverage;
        for (const auto* key : {"sources", "source_fact_sources"}) {
            for (const auto& row : snapshot->manifest().at(key)) {
                if (row.value("content_hash", "").empty()) continue;
                const auto name = row.at("path").get<std::string>();
                if (sources.contains(name) && sources.at(name).at("content_hash") != row.at("content_hash"))
                    throw Error("local-source-binding-invalid", "Published source bindings disagree.");
                sources[name] = row;
            }
        }
        const auto paths = core::expected_generated_relative_files(options);
        core::verify_snapshot_filesystem(config.out_dir, {paths.begin(), paths.end()});
        for (const auto& path : paths) artifacts.emplace(path, core::file_metadata(config.out_dir / path));
        // Link existence is an additional generation input, including targets
        // outside the project. Its bounded published inventory is not a scan.
        core::GeneratedInput input_links(options, config.out_dir / core::kGeneratedDir / "links.jsonl");
        for (std::string line; std::getline(input_links, line);) {
            const auto row = core::parse_json_text(line, "published link");
            const auto kind = row.value("kind", "");
            const auto state = row.value("status", "");
            if ((kind == "markdown-link" || kind == "path-reference") && (state == "ok" || state == "broken"))
                links.emplace(row.at("resolved_path").get<std::string>(), state == "ok");
        }
        if (!partial) discovery = std::make_unique<core::SourceDiscoveryIndex>(options, cache);
        if (fs::exists(cache)) {
            std::size_t entries = 0;
            for (const auto& entry : fs::directory_iterator(cache)) {
                if (++entries > 32) throw Error("resident-cache-budget", "Private search cache inventory exceeds its bound.");
                if (entry.path().extension() == ".sqlite") cache_files.emplace(entry.path(), core::file_metadata(entry.path()));
            }
        }
        guard();
        prepared_ms = core::elapsed_ms_since(started);
        prepared_at_ms = now_ms();
    }

    void guard() {
        verify_authority();
        changes->flush();
        if (!changes->healthy() || (!partial && changes->sequence() != epoch))
            throw Error("project-updating", "Source observation changed; background preparation is in progress.");
        const auto paths = core::expected_generated_relative_files(options);
        core::verify_snapshot_filesystem(config.out_dir, {paths.begin(), paths.end()});
        for (const auto& [path, metadata] : artifacts)
            if (core::file_metadata(config.out_dir / path) != metadata)
                throw Error("project-updating", "Published generation changed; background preparation is in progress.");
        for (const auto& [path, metadata] : cache_files)
            if (core::file_metadata(path) != metadata)
                throw Error("project-updating", "Derived search cache changed; background preparation is in progress.");
        for (const auto& [path, exists] : links)
            if (!partial && fs::exists(config.root / path) != exists)
                throw Error("project-updating", "A generation dependency changed; background preparation is in progress.");
    }

    Json wrap(const Json& result, const char* schema, const char* key) const {
        return {{"schema_version", schema}, {"binding_key", config.binding_key},
            {"project_id", config.binding.at("project_id")}, {"workspace_id", config.binding.at("workspace_id")},
            {"live_source_verified", true}, {"verification_scope", "selected-source-files"}, {key, result}};
    }
};

LocalQuerySession::LocalQuerySession(const Config& config, const fs::path& workspace, const fs::path& cache, const Json& status,
                                   std::function<void()> authority, const core::Profile* approved_profile)
    : impl_(std::make_unique<Impl>(config, workspace, cache, status, std::move(authority), approved_profile)) {}
LocalQuerySession::~LocalQuerySession() = default;

bool LocalQuerySession::observed_unchanged() {
    if (impl_->partial) {
        try { impl_->guard(); return true; } catch (const std::exception&) { return false; }
    }
    impl_->changes->flush();
    return impl_->changes->healthy() && impl_->changes->sequence() == impl_->epoch;
}

Json LocalQuerySession::description() const {
    Json result = {{"snapshot_id", impl_->snapshot->id()}, {"payload_bytes", impl_->snapshot->payload_bytes()},
        {"preparation_ms", impl_->prepared_ms}, {"prepared_at_ms", impl_->prepared_at_ms}, {"observation_epoch", impl_->epoch}};
    if (impl_->status.contains("source_coverage")) result["source_coverage"] = impl_->status.at("source_coverage");
    if (impl_->partial) {
        result["retained_profile"] = true;
        result["profile_revision"] = impl_->status.at("profile_revision");
    }
    return result;
}

Json LocalQuerySession::execute(const LocalProjectRequest& request) {
    auto& state = *impl_;
    if (request.root != state.config.root) throw Error("service-project-denied", "Resident project binding differs.");
    if (request.command == "checkpoint" || request.command == "resume") {
        state.guard();
        auto result = state.status;
        result.erase("readiness"); result["readiness_assessed"] = false;
        result.update(local_session_operation(state.options,state.workspace,request,[&] { state.guard(); }));
        result["command"] = request.command;
        result["freshness_contract"] = {{"mode","explicit-session-worktree-assessment"},{"whole_project_checked_during_request",true}};
        return result;
    }
    const auto started = core::SteadyClock::now();
    const auto generations = core::generation_execution_count();
    const auto publication = core::lock_index_for_read(state.config.out_dir);
    state.guard();
    const double admission_ms = core::elapsed_ms_since(started);
    auto result = state.status;
    result["command"] = request.command;
    result["snapshot_id"] = state.snapshot->id();
    result["synchronization"] = "reactive-background";
    result["freshness_contract"] = {{"mode", "background-qualified-published-generation"},
        {"whole_project_checked_during_request", false}, {"selected_sources_verified", true},
        {"observation_epoch", state.epoch}, {"prepared_at_ms", state.prepared_at_ms}};
    // Qualification is historical evidence, not a fresh audit result.
    result["background_readiness"] = result.at("readiness");
    result.erase("readiness");
    result["readiness_assessed"] = false;
    if (fs::exists(state.workspace / "watch")) {
        result["watch"] = local_watch_status(state.workspace / "watch");
    }
    const auto retrieval_started = core::SteadyClock::now();
    core::RetrievalSession session(state.snapshot, false);
    Json source;
    std::set<std::string> paths;
    std::set<std::string> omitted;
    const auto verified_file = [&](const std::string& path) {
        const auto found = state.sources.find(path);
        if (found == state.sources.end()) { omitted.insert(path); return false; }
        try {
            const auto content = core::read_bound_source(state.config.root, path,
                found->second.at("content_hash"), selected_bytes_limit);
            if (core::source_range_secret(content).found) { omitted.insert(path); return false; }
            return true;
        } catch (const core::CliError&) { omitted.insert(path); return false; }
    };
    double document_ms = 0, source_ms = 0, collection_ms = 0;
    if (request.command == "query") {
        const auto query = core::make_command_query_request(state.options.profile, {"--", request.query}, 4, 2);
        result["document_context"] = session.brief(query, core::BriefProjection::compact);
        document_ms = core::elapsed_ms_since(retrieval_started);
        const auto source_started = core::SteadyClock::now();
        if (!state.partial) {
            source = state.discovery->context_at_snapshot(state.options, request.query);
            collection_ms = source.at("collection_ms").get<double>();
        } else {
            auto& evidence = result["document_context"]["evidence"];
            Json retained = Json::array();
            for (const auto& row : evidence) {
                std::set<std::string> selected;
                collect_paths(row, state.sources, selected);
                bool safe = !selected.empty();
                for (const auto& path : selected) if (!verified_file(path)) safe = false;
                if (safe) retained.push_back(row);
            }
            evidence = std::move(retained);
            auto& document = result["document_context"];
            // These projections describe the unfiltered historical selection.
            // Omit them rather than claiming coverage or ranks for excluded files.
            for (const auto* key : {"ranked_paths", "verification_paths", "query_coverage", "identifier_scope"})
                document.erase(key);
            document["evidence_budget"]["returned_records"] = evidence.size();
            document["query_coverage"] = {{"state", "partial"}, {"scope", "verified-retained-evidence"}};
        }
        source_ms = core::elapsed_ms_since(source_started);
        collect_paths(result.at("document_context"), state.sources, paths);
        collect_paths(source, state.sources, paths);
    } else if (request.command == "inspect" || request.command == "read") {
        if (request.snapshot != state.snapshot->id())
            throw Error("source-snapshot-mismatch", "Follow-up belongs to a different published generation.");
        if (state.partial && request.path.empty())
            throw Error("partial-file-path-required", "Partial context reads require an explicit project-relative file path.");
        if (state.partial && !verified_file(request.path)) {
            source = Json{{"complete", false}, {"records", Json::array()}};
        } else if (request.command == "inspect") source = session.inspect(request.path);
        else {
            const bool file = request.record.empty();
            if (file) {
                const auto& manifest = state.snapshot->manifest();
                const auto& bindings = manifest.at("source_fact_sources");
                const bool scanned = std::any_of(bindings.begin(), bindings.end(), [&](const auto& row) {
                    return row.value("path", "") == request.path && row.value("status", "") == "scanned";
                });
                const auto& documents = manifest.at("sources");
                if (!scanned && std::any_of(documents.begin(), documents.end(), [&](const auto& row) {
                    return row.value("path", "") == request.path;
                })) throw Error("source-read-document-only", "This path is indexed as a document, not a scanned source file. Use document evidence or an authorized local file reader.");
            }
            source = session.read_selected(Json{{"schema_version", file ? "shin.source-read-request.v2" : "shin.source-read-request.v1"},
                {"snapshot_id", request.snapshot}, {file ? "file_paths" : "record_ids",
                Json::array({file ? request.path : request.record})}}.dump());
        }
        collect_paths(source, state.sources, paths);
    } else if (request.command != "status") throw Error("service-method-denied", "Resident reader only serves project reads.");
    const double retrieval_ms = core::elapsed_ms_since(retrieval_started);
    const auto verification_started = core::SteadyClock::now();
    if (paths.size() > selected_files_limit) throw Error("selected-source-budget", "Selected source verification exceeds its file budget.");
    std::uintmax_t bytes = 0;
    std::map<std::string, core::FileMetadata> observed;
    for (const auto& path : paths) {
        const auto& binding = state.sources.at(path);
        const auto size = binding.at("bytes").get<std::uintmax_t>();
        if (size > selected_bytes_limit - bytes) throw Error("selected-source-budget", "Selected source verification exceeds its byte budget.");
        bytes += size;
        const auto before = core::file_metadata(state.config.root / path);
        (void)core::read_bound_source(state.config.root, path, binding.at("content_hash"), selected_bytes_limit);
        observed.emplace(path, before);
    }
    if (request.command == "query" && state.partial)
        result["source_context"] = "Partial approved context: use the verified document evidence and snapshot-bound file reads. New or changed sources are not included.";
    else if (request.command == "query")
        result["source_context"] = source_view(state.wrap(source, "shin.observer-source-context.v1", "source_context"));
    else if (request.command == "inspect" && state.partial && !omitted.empty())
        result["source_inventory"] = {{"records", Json::array()}, {"complete", false}};
    else if (request.command == "read" && state.partial && !omitted.empty()) {
        result["source_context"] = "Source omitted: current bytes do not match the approved snapshot.";
        result["native_read_complete"] = false;
    } else if (request.command == "inspect")
        result["source_inventory"] = parse_json(source_view(state.wrap(source, "shin.observer-source-inspection.v1", "source_inspection")));
    else if (request.command == "read") {
        result["source_context"] = source_view(state.wrap(source, "shin.observer-source-read.v1", "source_read"), 32768);
        result["native_read_complete"] = source.at("complete");
    }
    state.guard();
    for (const auto& [path, metadata] : observed)
        if (core::profile_path_contains_symlink(state.config.root, path) || core::file_metadata(state.config.root / path) != metadata)
            core::throw_file_changed_during_verification(state.config.root / path);
    if (state.partial) {
        auto& coverage = result["source_coverage"];
        coverage["omitted_files"] = omitted.size();
        coverage["files"] = Json::array();
        for (const auto& path : omitted) {
            if (coverage["files"].size() >= 8) break;
            coverage["files"].push_back({{"path", path}, {"reason", "source-not-verified"}});
            if (coverage["files"].dump().size() > 4096) { coverage["files"].erase(coverage["files"].size() - 1); break; }
        }
        result["freshness_contract"]["mode"] = "approved-snapshot-selected-source-verification";
    }
    result["answer_complete"] = false;
    result["query_persisted"] = false;
    result["work_performed"] = {{"full_generations", core::generation_execution_count() - generations}, {"resident_hit", true},
        {"selected_files", paths.size()}, {"selected_source_bytes", bytes}, {"request_sequence", ++state.requests}};
    // Numerical read identity comes only from a successfully verified source.
    // Salt the file with this installation and project; unrelated generation
    // changes must not turn unchanged bytes into a new file version.
    if (request.command == "read" && !request.path.empty() && paths.size() == 1 && paths.contains(request.path) &&
        result.value("native_read_complete", Json()) == true) {
        const auto file = digest("shin-read-file-v1:" + request.home.string() + ":" + state.config.root.string() + ":" + request.path);
        result["work_performed"]["read_identity"] = {{"file_id",file},
            {"version_id",digest(file + ":" + state.sources.at(request.path).at("content_hash").get<std::string>())}};
    }
    result["phase_ms"] = {{"admission", admission_ms}, {"retrieval", retrieval_ms},
        {"documents", document_ms}, {"source", source_ms}, {"source_collection", collection_ms},
        {"final_verification", core::elapsed_ms_since(verification_started)}};
    if (result.dump().size() + 1 > 65536) throw Error("output-limit", "Resident response exceeds the bounded local view.");
    return result;
}
}
