#include "shin/server/preparation_service.h"
#include "shin/server/candidate_artifact.h"
#include "shin/server/route_files.h"
#include "shin/server/version.h"
#include "context_internal.h"
#include "generated_io.h"
#include "generated_snapshot.h"

#include <openssl/rand.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>

namespace shin::server {
namespace {
namespace core = shin::context;

std::string new_identity() {
    Digest random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        throw ServiceError(500, "random-identity-unavailable", "Cannot allocate a random identity.");
    return digest_hex(random);
}

bool snapshot_id_valid(const std::string& id) {
    return id.size() == 24 && id.starts_with("fnv1a64:") &&
        std::all_of(id.begin() + 8, id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

void sync_path(const std::filesystem::path& path, bool directory) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (directory ? O_DIRECTORY : 0));
    if (fd < 0) throw ServiceError(500, "artifact-sync-failed", "Cannot synchronize the prepared artifact.");
    const int result = ::fsync(fd);
    ::close(fd);
    if (result != 0) throw ServiceError(500, "artifact-sync-failed", "Cannot synchronize the prepared artifact.");
}

class LeasePulse {
public:
    LeasePulse(Store& store, std::uint64_t id, const std::string& owner, int seconds)
        : owner_(owner), thread_([this, &store, id, seconds](std::stop_token stop) {
            try {
                std::mutex mutex;
                std::condition_variable_any wake;
                std::unique_lock lock(mutex);
                while (!stop.stop_requested()) {
                    wake.wait_for(lock, stop, std::chrono::seconds(std::max(1, seconds / 3)), [] { return false; });
                    if (stop.stop_requested()) break;
                    if (store.renew_preparation(id, owner_, seconds)) continue;
                    lost_.store(true);
                    break;
                }
            } catch (...) {
                lost_.store(true);
            }
        }) {}
    bool lost() const { return lost_.load(); }
private:
    std::string owner_;
    std::atomic<bool> lost_{false};
    std::jthread thread_;
};

} // namespace

PreparationService::PreparationService(const Config& config, const RouteCatalog& catalog, Store& store)
    : config_(config), catalog_(catalog), store_(store),
      state_identity_(route_directory_identity(config.preparations.state_dir)), worker_id_("worker-" + new_identity().substr(0, 32)) {}

void PreparationService::start() {
    if (worker_.joinable()) throw ServiceError(500, "worker-already-started", "Preparation worker is already running.");
    if (config_.preparations.enabled) {
        observe("starting");
        worker_ = std::jthread([this](std::stop_token stop) { work(stop); });
    }
}

PreparationService::~PreparationService() {
    request_stop();
    if (worker_.joinable()) worker_.join();
}

void PreparationService::request_stop() {
    worker_.request_stop();
    wake_.notify_all();
}

std::string PreparationService::fingerprint(const BoundRoute& binding) const {
    return candidate_execution_fingerprint(binding.fingerprint, config_.preparations.state_dir, state_identity_,
        config_.retrieval.max_generation_bytes, config_.retrieval.max_file_bytes);
}

std::string PreparationService::artifact_namespace() const {
    return candidate_namespace(config_.preparations.state_dir, state_identity_);
}

void PreparationService::verify_state() const {
    if (route_directory_identity(config_.preparations.state_dir) != state_identity_)
        throw ServiceError(409, "preparation-root-changed", "Preparation state root changed.");
    struct stat state{};
    if (::lstat(config_.preparations.state_dir.c_str(), &state) != 0 ||
        state.st_uid != ::geteuid() || (state.st_mode & 0077) != 0)
        throw ServiceError(409, "preparation-root-unsafe", "Preparation state root is not private.");
}

Json PreparationService::enqueue(const Principal& principal, const std::string& project_id,
                                  const std::string& route_id, const Json& request, const std::string& key,
                                  const std::optional<WorkspaceReadRequest>& scope) {
    if (!scope && !principal.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
    if (!config_.preparations.enabled) throw ServiceError(503, "preparations-disabled", "Preparation jobs are disabled.");
    const auto admission = scope ? store_.preparation_admission(principal, project_id, *scope) : Json();
    if (!scope) (void)store_.get_project(principal, project_id);
    const auto& binding = catalog_.find(project_id, route_id);
    if (scope && binding.route.workspace_id != scope->workspace_id)
        throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
    const auto workspace = scope ? admission.at("workspace") : store_.get_workspace(principal, project_id, binding.route.workspace_id);
    catalog_.verify(binding, workspace);
    require_fields(request, {"expected_snapshot_id"});
    std::string expected;
    if (!request.at("expected_snapshot_id").is_null()) {
        expected = require_text(request, "expected_snapshot_id", 24);
        if (!snapshot_id_valid(expected)) throw ServiceError(400, "invalid-request", "Invalid expected snapshot identity.");
    }
    const auto route_fingerprint = fingerprint(binding);
    Json identity{{"operation", "prepare"}, {"project", project_id},
        {"route", route_id}, {"route_fingerprint", route_fingerprint}, {"expected_snapshot_id", expected}};
    if (scope) identity["workspace_scope"] = {{"tenant_id", scope->tenant_id}, {"workspace_id", scope->workspace_id}};
    const auto request_hash = digest_hex(sha256(identity.dump()));
    auto result = store_.enqueue_preparation(principal, project_id, route_id, route_fingerprint, expected, key,
        request_hash, config_.preparations.max_retained_jobs, config_.retrieval.max_generation_bytes,
        artifact_namespace(), new_identity, scope, admission);
    { std::lock_guard lock(mutex_); requested_ = true; }
    wake_.notify_all();
    return result;
}

void PreparationService::prepare(const Json& job, std::stop_token stop) {
    const auto id = job.at("id").get<std::uint64_t>();
    if (job.at("artifact_namespace") != artifact_namespace() || !job.at("artifact_key").is_string())
        throw ServiceError(409, "preparation-artifact-unbound", "Preparation has no matching artifact namespace.");
    verify_state();
    const auto key = job.at("artifact_key").get<std::string>();
    const CandidateArtifactLock artifact_lock(config_.preparations.state_dir, state_identity_, id, key,
                                               CandidateArtifactLock::Mode::create_writer);
    const auto& captured = job.at("workspace_admission");
    std::optional<WorkspaceReadRequest> scope;
    if (!captured.is_null()) {
        const auto& ownership = captured.at("authorization").at("ownership");
        scope = WorkspaceReadRequest{ownership.at("tenant_id").get<std::string>(), ownership.at("workspace_id").get<std::string>()};
    }
    const auto principal_id = job.at("principal_id").get<std::string>();
    Principal principal{principal_id, false, {}};
    if (!scope || captured.at("authorization").at("maintenance").get<bool>()) {
        const auto found = std::find_if(config_.credentials.begin(), config_.credentials.end(), [&](const Credential& credential) {
            return credential.principal.id == principal_id && credential.principal.admin;
        });
        if (found == config_.credentials.end())
            throw ServiceError(403, "preparation-principal-revoked", "Preparation principal is no longer authorized.");
        principal = found->principal;
    }
    // Personal jobs carry a principal locator, not a credential. The existing
    // live admission below rechecks current ownership and captured revisions.
    const auto& binding = catalog_.find(job.at("project_id").get<std::string>(), job.at("route_id").get<std::string>());
    const auto live_admission = [&] {
        if (!scope) return Json();
        const auto current = store_.preparation_admission(principal, binding.route.project_id, *scope);
        if (current != captured || scope->workspace_id != binding.route.workspace_id)
            throw ServiceError(409, "preparation-scope-changed", "Workspace authority changed since enqueue.");
        return current;
    };
    const auto admission = live_admission();
    const auto workspace = scope ? admission.at("workspace") :
        store_.get_workspace(principal, binding.route.project_id, binding.route.workspace_id);
    if (fingerprint(binding) != job.at("route_fingerprint").get<std::string>())
        throw ServiceError(409, "preparation-route-changed", "Preparation route changed since enqueue.");
    LeasePulse lease(store_, id, worker_id_, config_.preparations.lease_seconds);
    const auto guard = [&] {
        if (stop.stop_requested()) throw ServiceError(409, "preparation-stopped", "Worker is stopping.");
        if (lease.lost() || !store_.renew_preparation(id, worker_id_, config_.preparations.lease_seconds))
            throw ServiceError(409, "preparation-ownership-lost", "Preparation ownership was lost or cancelled.");
        verify_state();
        artifact_lock.verify();
        catalog_.verify(binding, workspace);
        (void)live_admission();
        const auto base = scope ? store_.scoped_active_route(principal, binding.route, *scope) :
            store_.active_route(principal, binding.route.project_id, binding.route.id);
        const auto snapshot_id = base.at("snapshot_id").is_null() ? "" : base.at("snapshot_id").get<std::string>();
        if (snapshot_id != job.at("expected_snapshot_id").get<std::string>() || base.at("revision") != job.at("expected_route_revision"))
            throw ServiceError(409, "preparation-base-changed", "The active snapshot differs from the requested base.");
    };
    guard();
    auto generated = core::generate_outputs(binding.options.profile, binding.options.root);
    std::uintmax_t bytes = 0;
    for (const auto& [path, content] : generated.files) {
        (void)path;
        if (content.size() > config_.retrieval.max_file_bytes || content.size() > config_.retrieval.max_generation_bytes - bytes)
            throw ServiceError(413, "preparation-artifact-too-large", "Candidate exceeds its artifact budget.");
        bytes += content.size();
    }
    if (generated.source_fact_capped_file_count != 0 || generated.source_fact_skipped_large_file_count != 0)
        throw ServiceError(409, "preparation-source-incomplete", "Candidate source extraction is incomplete.");
    guard();
    const auto artifact_root = config_.preparations.state_dir / candidate_artifact_name(id, key);
    if (::mkdir(artifact_root.c_str(), 0700) != 0)
        throw ServiceError(409, "preparation-artifact-exists", "Candidate location cannot be allocated exclusively.");
    auto options = binding.options;
    options.out_root = artifact_root;
    {
        shin::generated_io::IndexLock lock(artifact_root, shin::generated_io::LockMode::exclusive,
                                           shin::generated_io::LockTarget::writer);
        shin::generated_io::publish_snapshot(artifact_root, core::kGeneratedDir, generated.files);
    }
    generated.files.clear();
    const auto snapshot = core::GeneratedSnapshot::load(options,
        {config_.retrieval.max_generation_bytes, config_.retrieval.max_file_bytes});
    const auto health = core::inspect_generated_kb_health(options, true);
    if (health.counts.errors != 0 || health.counts.warnings != 0)
        throw ServiceError(409, "preparation-doctor-failed", "Candidate failed strict document and source safety checks.");
    for (const auto& path : core::expected_generated_relative_files()) sync_path(artifact_root / path, false);
    for (const auto& path : {artifact_root / core::kGeneratedDir, artifact_root / "docs" / "kb",
                            artifact_root / "docs", artifact_root, config_.preparations.state_dir}) sync_path(path, true);
    guard();
    const core::RetrievalSession admitted(snapshot, true);
    guard();
    // The artifact is durable before the database transition. An uncertain commit never triggers deletion.
    (void)store_.finish_preparation(id, worker_id_, "prepared", "", snapshot->id(), snapshot->payload_bytes());
}

void PreparationService::finish_failure(const Json& job, const std::string& reason) noexcept {
    try { (void)store_.finish_preparation(job.at("id").get<std::uint64_t>(), worker_id_, "failed", reason, "", 0); }
    catch (...) { uncertain_completions_.fetch_add(1); }
}

void PreparationService::work(std::stop_token stop) {
    while (!stop.stop_requested()) {
        try {
            store_.expire_preparations();
            verify_state();
            const auto job = store_.claim_preparation(worker_id_, config_.preparations.lease_seconds);
            if (!job.is_null()) {
                observe("working", "", job.at("id").get<std::uint64_t>());
                try { prepare(job, stop); }
                catch (const ServiceError& error) { finish_failure(job, error.code); }
                catch (const core::CliError& error) { finish_failure(job, error.code()); }
                catch (...) { finish_failure(job, "preparation-failed"); }
                continue;
            }
            observe("idle");
        } catch (const ServiceError& error) { observe("unavailable", error.code); }
        catch (...) { observe("unavailable", "preparation-worker-unavailable"); }
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, stop, std::chrono::milliseconds(500), [&] { return requested_; });
        requested_ = false;
    }
    observe("stopped");
}

void PreparationService::observe(const std::string& state, const std::string& error, std::uint64_t job_id) {
    std::lock_guard lock(mutex_);
    state_ = state;
    last_error_ = error;
    active_job_ = job_id;
}

Json PreparationService::status() const {
    std::lock_guard lock(mutex_);
    return {{"scope", "worker-process"}, {"enabled", config_.preparations.enabled}, {"state", state_},
            {"last_error_code", last_error_.empty() ? Json(nullptr) : Json(last_error_)},
            {"active_job_id", active_job_ == 0 ? Json(nullptr) : Json(active_job_)},
            {"uncertain_completion_count", uncertain_completions_.load()},
            {"activation_supported", true}};
}

} // namespace shin::server
