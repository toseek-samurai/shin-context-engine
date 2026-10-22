#include "shin/observer/local_service.h"
#include "shin/observer/local_publication.h"
#include "shin/observer/local_service_platform.h"
#include "shin/observer/local_rpc.h"
#include "shin/observer/local_worker_process.h"
#include "shin/observer/local_supervision.h"
#include "shin/observer/local_registry.h"
#include "shin/observer/model_usage.h"
#include "shin/observer/local_telemetry.h"
#include "shin/observer/resident_retention.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <thread>
#include <utility>

namespace shin::observer {
namespace {
using Clock = std::chrono::steady_clock;
using local_rpc::reply;
using local_rpc::failure;
using local_rpc::project_response;
using local_rpc::validate_request;
using local_rpc::string_field;
constexpr std::size_t request_limit = 8192, response_limit = 128 * 1024, connection_limit = 8, worker_limit = 2;
constexpr std::size_t resident_limit = kLocalResidentProjectLimit;
constexpr std::size_t waiting_limit = 4;
constexpr auto admission_wait_limit = std::chrono::seconds(5);
constexpr const char* refresh_retry_advice =
    " Continue useful work with direct sources. If Shin would help at a later task checkpoint, "
    "try one fresh query for the same project and use its new snapshot for inspect/read. "
    "Avoid polling or disabling Shin for the rest of the chat after one temporary failure.";
void require_not_cancelled(const ServiceCancelled& cancelled) {
    if (cancelled && cancelled()) throw Error("service-request-cancelled", "Local request was cancelled.");
}
bool resident_ready(const Json& status, const fs::path& root) {
    const auto& entries = status.at("resident_projects");
    return std::any_of(entries.begin(), entries.end(), [&](const Json& entry) {
        return entry.at("root") == root.string() && entry.at("ready") == true;
    });
}


class Connection {
public:
    explicit Connection(std::unique_ptr<ServiceChannel> input) : socket(std::move(input)) {}
    std::unique_ptr<ServiceChannel> socket;
    std::string method = "unknown", input, output, request_id, session_id, parent_request_id, model_id, snapshot;
    std::uint64_t admission_retries = 0;
    double wait_ms = 0;
    bool executed = false;
    fs::path root;
    bool metric_recorded = false, done = false, stop_reply = false, reply_sent = false;
    bool resident_pending = false, warm_pending = false, waited_for_resident = false, diagnostic = false;
    Clock::time_point deadline = Clock::now() + std::chrono::seconds(2), received{}, next_admission{};
};
struct Resident {
    explicit Resident(const fs::path& executable) : process(executable, LocalWorkerRole::resident) {}
    LocalWorkerProcess process;
    bool ready = false;
    std::string request_id, snapshot;
    Json preparation;
    Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);
    Clock::time_point last_used = Clock::now();
};

struct QualifiedProject {
    std::string root_identity, workspace_identity, snapshot;
    Json source_coverage = nullptr;
};

class Service {
public:
    Service(const LocalProjectRequest& input, const std::vector<fs::path>& roots, const fs::path& binary, bool registered, bool managed)
        : home(fs::absolute(input.home).lexically_normal()), directory(local_service_directory(home)), executable(fs::canonical(binary)),
          registry_mode(registered), supervised(managed) {
        executable_id = service_executable_identity(executable);
        ensure_private_directory(directory);
        owner = std::make_unique<StateStore>(directory, true);
        for (const auto& root : roots) {
            const auto canonical = fs::canonical(root);
            bindings.emplace(canonical.string(), std::pair{directory_identity(canonical),
                directory_identity(home / "projects" / digest(canonical.string()), true)});
        }
        endpoint = make_service_endpoint(home);
        telemetry.open_history(directory / "history");
    }

    ~Service() {
        connections.clear();
        residents.clear();
        telemetry.close_history();
        endpoint.reset();
    }

    void poll() {
        poll_residents();
        trim_resident_memory();
        std::optional<std::uint64_t> rss = std::uint64_t{0};
        const auto account = [&](const std::optional<std::uint64_t>& bytes) {
            if (rss && bytes) *rss += *bytes;
            else rss.reset();
        };
        for (const auto& [root, reader] : residents) { (void)root; account(reader->process.rss_bytes()); }
        if (refresh_worker) account(refresh_worker->rss_bytes());
        telemetry.sample(active_workers(), rss);
        endpoint->verify();
        for (std::size_t count = 0; count < connection_limit; ++count) {
            auto channel = endpoint->accept();
            if (!channel) break;
            if (connections.size() == connection_limit) {
                telemetry.request_result({}, "unknown", {{"stage", "admission"}, {"error_code", "service-connection-capacity"}});
                continue;
            }
            connections.push_back(std::make_unique<Connection>(std::move(channel)));
        }
        for (auto& client : connections) process(*client);
        std::erase_if(connections, [](const auto& client) { return client->done; });
    }

    bool stopping = false;
    std::uint64_t undelivered_stop_replies = 0;
    void drain_stop_replies() {
        // A large telemetry reply may require several writes. Do not destroy the
        // socket with the shutdown acknowledgement still only partially sent.
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        for (auto& client : connections) {
            if (!client->stop_reply || client->done) continue;
            try {
                if (!client->output.empty()) service_write(*client->socket, client->output, deadline);
                client->socket->finish_reply(deadline, {});
            }
            catch (const Error&) { ++undelivered_stop_replies; }
            client->output.clear();
            client->done = true;
        }
    }
    LocalTelemetry telemetry;
    Json refresh(const LocalProjectRequest& request, const std::function<bool()>& stopped) {
        const auto started = Clock::now();
        const auto wall_started = std::chrono::system_clock::now();
        LocalSuspendObservation suspension;
        Json work = Json::object();
        const std::set<std::string> refresh_stages{"admission", "observation", "candidate-build", "candidate-validation",
            "publication", "qualification", "source-cache", "candidate-source-cache", "source-cache-transfer", "final-verification"};
        Json phases = Json::object();
        std::string last_stage;
        auto last_progress = started;
        auto last_elapsed_ms = 0.0;
        const auto finish_stage = [&](Clock::time_point now, std::optional<double> measured_ms) {
            if (last_stage.empty()) return;
            const auto duration = measured_ms.value_or(
                std::chrono::duration<double, std::milli>(now - last_progress).count());
            const auto prior = phases.value(last_stage, 0.0);
            phases[last_stage] = prior + duration;
        };
        const auto persist = [&](const std::string& outcome, const std::string& code, const std::string& phase) {
            finish_stage(Clock::now(), {});
            const double sleep_ms = suspension.suspended_ms();
            const double wall_ms = std::max(sleep_ms, std::chrono::duration<double, std::milli>(
                std::chrono::system_clock::now() - wall_started).count());
            try { telemetry.refresh_result(request.root, {{"outcome", outcome},
                {"duration_ms", wall_ms},
                {"active_ms", suspension.available() ? Json(std::max(0.0, wall_ms - sleep_ms)) : Json(nullptr)},
                {"sleep_ms", suspension.available() ? Json(sleep_ms) : Json(nullptr)},
                {"error_code", code}, {"phase", phase}, {"last_stage", last_stage}, {"phases", phases}, {"work", work}}); }
            catch (...) { return; /* Diagnostics never change project authority or refresh results. */ }
        };
        try {
        if (service_executable_identity(executable) != executable_id)
            throw Error("service-executable-changed", "Restart after replacing the native executable.");
        LocalWorkerProcess worker(executable, LocalWorkerRole::refresh);
        refresh_worker = &worker;
        struct ResetRefresh { LocalWorkerProcess*& value; ~ResetRefresh() { value = nullptr; } } reset_refresh{refresh_worker};
        const Json payload{{"schema_version", "shin.local-refresh.v1"}, {"version", SHIN_OBSERVER_VERSION},
            {"core_version", SHIN_VERSION}, {"root", request.root.string()}, {"home", request.home.string()},
            {"root_identity", request.maintenance_root_identity},
            {"workspace_identity", request.maintenance_workspace_identity},
            {"worktree", request.maintenance_worktree}, {"diagnostics", true}};
        const auto pump = [&] { poll(); return stopping || stopped(); };
        worker.send(payload, Clock::now() + std::chrono::seconds(2), pump);
        // Only one refresh is active. Keep accepting independent queries and stop
        // requests even when this project's Core call waits on an index lock.
        Json result;
        const auto receive_progress = [&](const Json& frame) {
            if (!frame.is_object() || frame.value("kind", "") != "progress") return;
            const auto stage = frame.value("stage", "");
            const auto elapsed = frame.value("elapsed_ms", Json());
            if (!refresh_stages.contains(stage) || !elapsed.is_number()) return;
            const auto value = elapsed.get<double>();
            if (!std::isfinite(value) || value < last_elapsed_ms || value > 60000.0) return;
            const auto now = Clock::now();
            finish_stage(now, std::max(0.0, value - last_elapsed_ms));
            last_stage = stage;
            last_elapsed_ms = value;
            last_progress = now;
        };
        const auto receive_deadline = Clock::now() + std::chrono::seconds(60);
        try {
            result = worker.receive(receive_deadline, pump, receive_progress);
        } catch (const Error& error) {
            if (error.code == "service-timeout") throw Error("refresh-timeout", "Refresh exceeded its 60-second worker deadline.");
            throw;
        }
        if (result.contains("error"))
            throw Error(result.at("error").at("code"), result.at("error").at("message"),
                        result.at("error").value("diagnostic", Json()));
        if (!result.is_object() || !result.contains("ok") || !result.at("ok").is_boolean())
            throw Error("refresh-worker-failed", "Invalid refresh worker response.");
        // Preparation is never performed in a caller's query path. The child
        // independently qualifies its generation before advertising readiness.
        worker.stop();
        work = result.value("refresh_work", Json::object());
        if (result.at("ok") == true && (result.value("phase", "") == "current" || result.value("phase", "") == "partial")) {
            const auto snapshot = result.value("snapshot_id", result.value("observation", Json::object()).value("snapshot_id", ""));
            const auto existing = residents.find(request.root.string());
            const bool partial = result.value("phase", "") == "partial";
            if (existing != residents.end() && existing->second->ready &&
                (existing->second->preparation.value("retained_profile", false) != partial ||
                 (partial && existing->second->preparation.value("profile_revision", Json()) != result.value("profile_revision", Json()))))
                discard_resident(request.root.string(), "project-updating");
            qualified[request.root.string()] = {request.maintenance_root_identity, request.maintenance_workspace_identity, snapshot};
            resident_errors.erase(request.root.string());
            const auto demand = reader_requests.find(request.root.string());
            prepare_resident(request, snapshot, demand != reader_requests.end() && demand->second > Clock::now());
        } else {
            qualified.erase(request.root.string());
            discard_resident(request.root.string(), "project-updating");
        }
        const auto code = result.contains("error") ? result.at("error").value("code", "refresh-failed") :
            (result.value("ok", false) ? std::string{} : std::string("refresh-failed"));
        persist(code.empty() ? "success" : "error", code, result.value("phase", "unknown"));
        return result;
        } catch (const Error& error) {
            persist(error.code == "refresh-suspended" ? "paused" : "error", error.code,
                    error.code == "refresh-suspended" ? "coalescing" : "unknown");
            throw;
        } catch (...) {
            persist("error", "refresh-worker-failed", "unknown");
            throw;
        }
    }
    void enrollment_result(const LocalProjectRequest& request, const std::string& error) {
        if (request.root.empty()) { registry_error = error; return; }
        const auto root = request.root.string();
        if (!error.empty()) { qualified.erase(root); discard_resident(root, error); enrollment_errors[root] = error; return; }
        bindings.emplace(root, std::pair{request.maintenance_root_identity, request.maintenance_workspace_identity});
        enrollment_errors.erase(root);
    }
    void retire_project(const fs::path& root) {
        discard_resident(root.string(), "project-detached");
        bindings.erase(root.string());
        enrollment_errors.erase(root.string());
        resident_errors.erase(root.string());
        project_blockers.erase(root.string());
        qualified.erase(root.string());
        reader_requests.erase(root.string());
        telemetry.detach(root);
    }
    void project_state(const fs::path& root, const Json& state) {
        const auto phase = state.value("phase", "");
        const auto last = state.value("last_result", Json());
        const auto retry = last.is_object() ? last.value("retry_reason", "") : std::string{};
        if (phase == "current" || phase == "partial" || (phase == "coalescing" &&
            (retry == "refresh-superseded" || retry == "file-changed-during-verification")))
            project_blockers.erase(root.string());
        if (phase == "blocked") {
            const auto result = state.value("last_result", Json::object());
            const auto error = result.value("error", Json::object());
            const auto code = validation_identifier(error.value("code", Json()));
            const auto diagnostic = error.value("diagnostic", Json());
            const auto reason = validation_failure_reason(diagnostic);
            std::string message;
            if (code == "refresh-timeout") {
                message = std::string("This refresh exceeded its worker deadline. The service will retry qualification automatically.") + refresh_retry_advice;
            } else if (code == "route-revision-conflict") {
                message = "Project settings changed and require owner review. Previous index preserved. "
                    "For profile changes, use View changes in Shin Monitor or read-only shin-ce profile-review "
                    "with this project's verified root and home. Marker or route configuration changes require "
                    "separate operator review. Repeating queries or reconnecting MCP does not approve changes. "
                    "Report this blocker and continue useful work with direct sources. After explicit acceptance, "
                    "wait for background qualification and verify readiness with a fresh query.";
            } else if (code == "access-denied") {
                message = "Shin cannot read part of this project because access was denied. Ask the operator to review "
                    "file access; Shin will check again automatically. Previous index preserved. Do not change permissions automatically.";
            } else if (code == "resource-exhausted") {
                message = "Shin could not obtain the system resources needed to read this project. Ask the operator "
                    "to review available memory and file handles. Previous index preserved; project settings need no automatic change.";
            } else if (code == "source-io-error") {
                message = "Shin could not read project data. Ask the operator to check storage availability and the "
                    "reported diagnostic. Previous index preserved; rebuilding the index is not a repair for a read failure.";
            } else if (reason == "missing-required-path") {
                message = "Current project context is unavailable: a required document was not found in search results. "
                    "This does not show that the document is missing from disk.";
                for (const auto& path : validation_failure_expected_paths(diagnostic))
                    message += " Expected document: " + path + ".";
                message += " After project changes, Shin will check again. If the file is present and the failure persists, "
                    "save this diagnostic for Shin support; validation settings do not need changes. Previous index preserved.";
            } else {
                message = "Current project context is unavailable: refresh check failed" +
                    (reason.empty() ? std::string{} : " (" + reason + ")") +
                    ". Previous index preserved. Report this blocker to the user; direct-source fallback is not restored Shin readiness.";
            }
            project_blockers[root.string()] = error_json(Error(code.empty() ? "project-check-failed" : code,
                message, error.value("diagnostic", Json())));
        }
        // Sleep/retry is not successful qualification. Keep an earlier blocker
        // visible while retrying, just as it remains enforced on the read path.
        auto visible = state;
        if (const auto blocker = project_blockers.find(root.string());
            blocker != project_blockers.end() && phase != "blocked") {
            visible["phase"] = "blocked";
            visible["last_result"] = {{"ok", false}, {"error", blocker->second}};
        }
        telemetry.record(root, visible);
        if (phase != "checking" && phase != "current" && phase != "partial") {
            qualified.erase(root.string());
            discard_resident(root.string(), "project-updating");
        }
    }
    void retire_detached_errors() {
        std::vector<std::string> retired;
        for (const auto& [root, error] : enrollment_errors) {
            (void)error;
            try {
                if (local_project_lifecycle(home / "projects" / digest(root)).phase != "active") retired.push_back(root);
            } catch (const std::exception&) {
                // An unreadable lifecycle retains its diagnostic and active isolation.
                continue;
            }
        }
        for (const auto& root : retired) retire_project(root);
    }
    std::set<fs::path> preparing_projects;
    std::vector<fs::path> reader_demand() {
        std::erase_if(reader_requests, [&](const auto& entry) {
            return entry.second <= Clock::now() || qualified.contains(entry.first) || !bindings.contains(entry.first);
        });
        std::vector<fs::path> roots;
        for (const auto& [root, until] : reader_requests) { (void)until; roots.emplace_back(root); }
        return roots;
    }
private:
    void record_result(Connection& client, Json metric) {
        if (client.metric_recorded) return;
        metric["service_duration_ms"] = client.received == Clock::time_point{} ? Json(nullptr) :
            Json(std::chrono::duration<double, std::milli>(Clock::now() - client.received).count());
        metric["reader_state"] = client.waited_for_resident ? "cold" : "warm";
        metric["purpose"] = client.diagnostic ? "diagnostic" : "unclassified";
        metric["stage"] = client.executed ? "execution" : "admission";
        metric["request_id"] = client.request_id.empty() ? "" : digest(client.request_id);
        metric["session_id"] = client.session_id;
        metric["model_id"] = client.model_id;
        metric["parent_request_id"] = client.parent_request_id;
        metric["generation_id"] = client.snapshot.empty() ? "" : digest(client.snapshot);
        metric["wait_ms"] = client.wait_ms;
        metric["admission_retries"] = client.admission_retries;
        telemetry.request_result(client.root, client.method, metric);
        client.metric_recorded = true;
    }

    void discard_resident(const std::string& root, const std::string& code) {
        for (auto& client : connections) {
            if ((!client->resident_pending && !client->warm_pending) || client->root.string() != root) continue;
            // A warming request has not executed. Keep its original frame and
            // deadline across transient refresh invalidation; all admission
            // checks run again. Never replay an already dispatched request.
            if (client->warm_pending && code == "project-updating") continue;
            client->resident_pending = false;
            client->warm_pending = false;
            client->output = failure(client->request_id, code, "Project search is preparing in the background; retry shortly.").dump() + '\n';
            record_result(*client, {{"error_code", code}});
        }
        residents.erase(root);
    }

    void prepare_resident(const LocalProjectRequest& request, const std::string& snapshot, bool demanded = false) {
        const auto root = request.root.string();
        const auto found = residents.find(root);
        // After pressure, only demand warms new roots. Background checks still
        // observe/qualify every project without repeatedly undoing reclamation.
        if (found == residents.end() && resident_memory_limited && !demanded) return;
        auto last_used = Clock::now();
        if (found != residents.end()) {
            if (found->second->snapshot == snapshot || !found->second->ready) return;
            last_used = found->second->last_used;
            discard_resident(root, "project-updating");
        }
        if (residents.size() >= resident_limit) {
            if (!demanded) return;
            auto victim = residents.end();
            for (auto it = residents.begin(); it != residents.end(); ++it) {
                if (!it->second->ready || !it->second->request_id.empty() ||
                    std::any_of(connections.begin(), connections.end(), [&](const auto& client) {
                        return client->warm_pending && client->root.string() == it->first;
                    })) continue;
                if (victim == residents.end() || it->second->last_used < victim->second->last_used) victim = it;
            }
            if (victim == residents.end()) return;
            residents.erase(victim);
            ++resident_evictions;
        }
        if (service_executable_identity(executable) != executable_id)
            throw Error("service-executable-changed", "Restart after replacing the native executable.");
        auto reader = std::make_unique<Resident>(executable);
        reader->snapshot = snapshot;
        reader->last_used = demanded ? Clock::now() : last_used;
        const Json payload{{"schema_version", "shin.local-resident.v1"}, {"version", SHIN_OBSERVER_VERSION},
            {"core_version", SHIN_VERSION}, {"root", root}, {"home", home.string()},
            {"root_identity", request.maintenance_root_identity}, {"workspace_identity", request.maintenance_workspace_identity}};
        reader->process.send(payload, Clock::now() + std::chrono::seconds(1));
        residents.emplace(root, std::move(reader));
    }

    void poll_residents() {
        std::map<std::string, std::string> remove;
        for (auto& [root, resident] : residents) {
            auto& reader = *resident;
            try {
                const auto output = reader.process.poll();
                if (output.frame) {
                    const auto frame = parse_json(*output.frame);
                    const auto kind = frame.at("kind").get<std::string>();
                    if (kind == "ready" && !reader.ready && reader.request_id.empty()) {
                        reader.ready = true;
                        reader.preparation = frame.at("preparation");
                        reader.snapshot = reader.preparation.at("snapshot_id");
                        const auto selected = qualified.find(root);
                        if (selected != qualified.end() && selected->second.snapshot == reader.snapshot)
                            selected->second.source_coverage = reader.preparation.value("source_coverage", Json());
                        resident_errors.erase(root);
                    } else if (kind == "response" && reader.ready && !reader.request_id.empty()) {
                        const auto& response = frame.at("response");
                        if (response.at("request_id") != reader.request_id || response.at("schema_version") != "shin.local-rpc.v1")
                            throw Error("resident-frame-invalid", "Resident response identity differs.");
                        for (auto& client : connections) {
                            if (!client->resident_pending || client->root.string() != root || client->request_id != reader.request_id) continue;
                            auto delivered = response;
                            if (delivered.contains("result") && delivered.at("result").contains("work_performed")) {
                                delivered["result"]["work_performed"]["resident_cache_hit"] = !client->waited_for_resident;
                                delivered["result"]["work_performed"]["resident_warmed"] = client->waited_for_resident;
                            }
                            client->output = delivered.dump() + '\n';
                            if (client->output.size() > response_limit) throw Error("resident-frame-invalid", "Resident response exceeds its bound.");
                            client->resident_pending = false;
                            if (response.contains("result")) client->snapshot = response.at("result").value("snapshot_id", client->snapshot);
                            record_result(*client, frame.at("metric"));
                        }
                        reader.request_id.clear();
                        reader.last_used = Clock::now();
                        if (!frame.at("reusable").get<bool>()) remove[root] = "project-updating";
                    } else if (kind == "invalidated" || kind == "preparation-failed") {
                        remove[root] = frame.value("code", "project-updating");
                    } else throw Error("resident-frame-invalid", "Unexpected resident frame.");
                }
                if (output.ended || (!reader.ready && Clock::now() >= reader.deadline))
                    remove.try_emplace(root, output.ended ? "resident-disconnected" : "resident-preparation-timeout");
            } catch (const Error& error) {
                remove[root] = error.code == "service-disconnected" ? "resident-disconnected" : "resident-frame-invalid";
            } catch (...) { remove[root] = "resident-frame-invalid"; }
        }
        for (const auto& [root, code] : remove) {
            qualified.erase(root);
            resident_errors[root] = code;
            discard_resident(root, code);
        }
    }

    std::size_t active_workers() const {
        return static_cast<std::size_t>(std::count_if(connections.begin(), connections.end(),
            [](const auto& client) { return client->resident_pending; }));
    }

    std::size_t waiting_requests() const {
        return static_cast<std::size_t>(std::count_if(connections.begin(), connections.end(),
            [](const auto& client) { return client->warm_pending; }));
    }

    void dispatch(Connection& client, const Json& request) {
        validate_request(request);
        const auto id = request.at("request_id").get<std::string>();
        const auto command = request.at("command").get<std::string>();
        client.request_id = id;
        client.method = command;
        client.session_id = request.value("session_id", "");
        client.model_id = request.value("model_id", "");
        client.parent_request_id = request.value("parent_request_id", "");
        client.snapshot = request.at("snapshot").get<std::string>();
        if (command == "service-status" || command == "service-stop") {
            auto value = reply(id);
            value["ok"] = true;
            value["result"] = {{"schema_version", "shin.local-service-status.v1"}, {"tool", "shin-ce"},
                {"version", SHIN_OBSERVER_VERSION}, {"core_version", SHIN_VERSION},
                {"ok", true}, {"command", command}, {"service_running", true},
                {"query_workers", active_workers()}, {"max_query_workers", worker_limit},
                {"waiting_requests", waiting_requests()}, {"max_waiting_requests", waiting_limit},
                {"admission_wait_limit_ms", std::chrono::duration_cast<std::chrono::milliseconds>(admission_wait_limit).count()},
                {"max_connections", connection_limit}, {"projects", Json::array()},
                {"resident_projects", Json::array()}, {"max_resident_projects", resident_limit},
                {"max_registered_projects", kLocalServiceProjectLimit}, {"qualified_projects", Json::array()},
                {"resident_evictions", resident_evictions}, {"resident_warm_requests", resident_warm_requests},
                {"resident_retention_bytes", kResidentRetentionBytes}, {"resident_memory_evictions", resident_memory_evictions},
                {"resident_memory_limited", resident_memory_limited},
                {"resident_errors", resident_errors},
                {"enrollment_mode", registry_mode ? "registry" : "explicit"}, {"instance_id", instance_id},
                {"supervision", supervised ? local_service_manager() : "unmanaged"},
                {"registry_error", registry_error}, {"enrollment_errors", enrollment_errors},
                {"readiness_assessed", false}, {"stop_requested", command == "service-stop"}};
            // A stop acknowledgement must not depend on dashboard/history size.
            if (command == "service-status") {
                value["result"]["telemetry"] = telemetry.snapshot();
                for (auto& project : value["result"]["telemetry"]["project_states"]) {
                    const auto found = residents.find(project.at("root").get<std::string>());
                    project["query_ready"] = found != residents.end() && found->second->ready;
                    if (project["query_ready"] == true && found->second->preparation.contains("source_coverage"))
                        project["source_coverage"] = found->second->preparation.at("source_coverage");
                    const auto root = project.at("root").get<std::string>();
                    const auto selected = qualified.find(root);
                    if (selected != qualified.end() && !selected->second.source_coverage.is_null())
                        project["source_coverage"] = selected->second.source_coverage;
                    project["query_phase"] = project.at("query_ready") == true ? "ready" :
                        (found == residents.end() && qualified.contains(root) ? "standby" : "preparing");
                    const auto error = resident_errors.find(project.at("root").get<std::string>());
                    project["query_error_code"] = error == resident_errors.end() ? "" : error->second;
                    if (project.at("query_ready") == false && error != resident_errors.end() &&
                        error->second != "project-updating" && error->second != "resident-disconnected") {
                        project["active_issue"] = true;
                        project["error_code"] = error->second;
                    }
                    const auto blocker = project_blockers.find(root);
                    if (blocker != project_blockers.end()) {
                        project["active_issue"] = true;
                        project["error_code"] = blocker->second.at("code");
                        project["query_phase"] = "blocked";
                        project["validation_diagnostic"] = blocker->second.value("diagnostic", Json());
                    }
                }
            }
            for (const auto& [root, identities] : bindings) {
                (void)identities;
                value["result"]["projects"].push_back(root);
            }
            for (const auto& [root, reader] : residents) {
                const auto rss = reader->process.rss_bytes();
                value["result"]["resident_projects"].push_back({{"root", root}, {"ready", reader->ready},
                    {"busy", !reader->request_id.empty()}, {"preparation", reader->preparation},
                    {"pid", reader->process.process_id()}, {"rss_bytes", rss ? Json(*rss) : Json(nullptr)}});
            }
            for (const auto& [root, project] : qualified)
                value["result"]["qualified_projects"].push_back({{"root", root}, {"snapshot_id", project.snapshot}});
            value["result"]["preparing_projects"] = Json::array();
            for (const auto& root : preparing_projects) value["result"]["preparing_projects"].push_back(root.string());
            if (command == "service-status") bound_local_service_status(value["result"]);
            client.output = value.dump() + '\n';
            if (client.output.size() > response_limit)
                throw Error("service-frame-too-large", "Local service status exceeds its response bound.");
            if (command == "service-stop") { client.stop_reply = true; stopping = true; }
            return;
        }
        const auto found = bindings.find(request.at("root").get<std::string>());
        if (found == bindings.end()) throw Error("service-project-denied", "Project was not enrolled in this service.");
        client.root = found->first; // Attribute only a validated, enrolled root.
        require_local_project_active(home / "projects" / digest(found->first));
        if (service_executable_identity(executable) != executable_id)
            throw Error("service-executable-changed", "Restart the service after replacing the native executable.");
        if (const auto error = enrollment_errors.find(found->first); error != enrollment_errors.end())
            throw Error(error->second, "Project enrollment requires attention.");
        if (const auto error = resident_errors.find(found->first); error != resident_errors.end() &&
            error->second != "project-updating" && error->second != "resident-disconnected")
            throw Error(error->second, "Project reader preparation requires attention.");
        const auto blocker = project_blockers.find(found->first);
        if (blocker != project_blockers.end())
            throw Error(blocker->second.at("code"), blocker->second.at("message"),
                        blocker->second.value("diagnostic", Json()));
        if (std::any_of(connections.begin(), connections.end(), [&](const auto& other) {
            return other.get() != &client && (other->warm_pending || other->resident_pending) && other->root.string() == found->first;
        })) throw Error("service-busy", "This project's reader is serving another request; retry shortly.");
        // Waiting has its own bound; it must not consume another project's
        // execution slot. Keep space for active queries and service control.
        if (!client.warm_pending && waiting_requests() >= waiting_limit &&
            (!residents.contains(found->first) || !residents.at(found->first)->ready))
            throw Error("service-busy", "Local preparation waiting capacity is full; retry later.");
        if (!residents.contains(found->first)) {
            const auto available = qualified.find(found->first);
            if (available != qualified.end()) {
                LocalProjectRequest prepare;
                prepare.root = found->first; prepare.home = home;
                prepare.maintenance_root_identity = available->second.root_identity;
                prepare.maintenance_workspace_identity = available->second.workspace_identity;
                prepare_resident(prepare, available->second.snapshot, true);
            }
        }
        const auto resident = residents.find(found->first);
        if (resident == residents.end() || !resident->second->ready) {
            // Retain one bounded scheduling hint after a caller times out or
            // disconnects, so qualification can finish once another refresh ends.
            // The 100ms admission retries must not extend this lifetime.
            if (!qualified.contains(found->first))
                reader_requests.try_emplace(found->first, client.received + std::chrono::seconds(30));
            if (!client.warm_pending) {
                ++resident_warm_requests;
                client.deadline = client.received + admission_wait_limit;
            }
            client.warm_pending = true;
            client.next_admission = Clock::now() + std::chrono::milliseconds(100);
            client.waited_for_resident = true;
            client.root = found->first;
            client.method = command;
            client.request_id = id;
            return;
        }
        if (active_workers() >= worker_limit) {
            if (client.warm_pending) {
                client.next_admission = Clock::now() + std::chrono::milliseconds(100);
                return; // Keep its original deadline and never replay executed work.
            }
            throw Error("service-busy", "Local query capacity is full; retry later.");
        }
        auto& reader = *resident->second;
        if (!reader.request_id.empty()) throw Error("service-busy", "This project's reader is serving another request; retry shortly.");
        try { reader.process.send(request, Clock::now() + std::chrono::seconds(1)); }
        catch (...) { discard_resident(found->first, "resident-disconnected"); throw; }
        client.executed = true;
        if (client.waited_for_resident)
            client.wait_ms = std::chrono::duration<double, std::milli>(Clock::now() - client.received).count();
        reader.request_id = id;
        reader.last_used = Clock::now();
        client.request_id = id;
        client.resident_pending = true;
        client.deadline = client.received + std::chrono::seconds(30);
        client.warm_pending = false;
        client.method = command;
        client.root = found->first;
        if (command != "status") ++telemetry.requests;
    }

    void process(Connection& client) {
        if (client.warm_pending) {
            const bool expired = Clock::now() >= client.deadline;
            if (client.socket->input_or_closed() || expired) {
                client.wait_ms = std::chrono::duration<double, std::milli>(Clock::now() - client.received).count();
                record_result(client, {{"error_code", expired ? "project-updating" : "client-disconnected"}});
                client.warm_pending = false;
                if (!expired) { client.done = true; return; }
                client.output = failure(client.request_id, "project-updating",
                    std::string("Project is still updating after bounded admission waiting.") + refresh_retry_advice).dump() + '\n';
                client.deadline = Clock::now() + std::chrono::seconds(1); // Flush explicit refusal.
            } else {
                if (Clock::now() < client.next_admission) return;
                ++client.admission_retries;
                try { dispatch(client, parse_json(client.input)); }
                catch (const Error& error) {
                    client.warm_pending = false;
                    client.output = failure(client.request_id, error.code, error.what(), error.diagnostic).dump() + '\n';
                } catch (const std::exception&) {
                    client.warm_pending = false;
                    client.output = failure(client.request_id, "service-request-failed", "Cannot prepare the requested project reader.").dump() + '\n';
                }
                if (client.warm_pending || client.resident_pending) return;
                client.wait_ms = std::chrono::duration<double, std::milli>(Clock::now() - client.received).count();
                record_result(client, {{"error_code", parse_json(client.output).at("error").at("code")}});
            }
        }
        if (client.resident_pending) {
            if (client.socket->input_or_closed() || Clock::now() >= client.deadline) {
                discard_resident(client.root.string(), Clock::now() >= client.deadline ? "service-timeout" : "client-disconnected");
                client.done = true;
            }
            return;
        }

        if (Clock::now() >= client.deadline) {
            if (client.output.empty() && !client.reply_sent)
                record_result(client, {{"error_code", "service-input-timeout"}});
            client.done = true;
            return;
        }
        if (client.reply_sent) {
            try { client.done = client.socket->reply_consumed(); }
            catch (const Error&) { client.done = true; }
            return;
        }
        if (client.output.empty()) {
            std::optional<std::string> bytes;
            try { bytes = client.socket->read(Clock::now()); }
            catch (const Error&) { client.done = true; return; }
            if (!bytes) return;
            if (bytes->empty()) { client.done = true; return; }
            client.input += *bytes;
            std::string id;
            try {
                if (client.input.size() > request_limit) throw Error("service-frame-too-large", "Request exceeds 8192 bytes.");
                const auto end = client.input.find('\n');
                if (end == std::string::npos) return;
                if (end != client.input.size() - 1) throw Error("service-frame-invalid", "Only one request per connection is supported.");
                const auto request = parse_json(client.input);
                client.received = Clock::now();
                client.diagnostic = request.is_object() && request.value("diagnostic", Json(false)) == true;
                if (request.is_object() && request.contains("request_id") && request.at("request_id").is_string())
                    id = string_field(request, "request_id", 96);
                dispatch(client, request);
            } catch (const Error& error) {
                record_result(client, {{"error_code", error.code}});
                client.output = failure(id, error.code, error.what(), error.diagnostic).dump() + '\n';
            } catch (const std::exception&) {
                record_result(client, {{"error_code", "service-request-failed"}});
                client.output = failure(id, "service-request-failed", "Local request could not be admitted.").dump() + '\n';
            }
        }
        if (!client.output.empty()) {
            try {
                const auto count = client.socket->write(client.output);
                if (count) {
                    if (!*count) { client.done = true; return; }
                    client.output.erase(0, *count);
                }
                if (client.output.empty()) {
                    client.reply_sent = true;
                    client.done = client.socket->reply_consumed();
                }
            } catch (const Error&) { client.done = true; }
        }
    }

    fs::path home, directory, executable;
    std::string executable_id;
    std::unique_ptr<ServiceEndpoint> endpoint;
    std::unique_ptr<StateStore> owner;
    std::map<std::string, std::pair<std::string, std::string>> bindings;
    std::vector<std::unique_ptr<Connection>> connections;
    bool registry_mode;
    bool supervised;
    std::string instance_id = digest(std::to_string(service_process_id()) + ":" + std::to_string(Clock::now().time_since_epoch().count()));
    std::string registry_error;
    std::map<std::string, std::string> enrollment_errors;
    std::map<std::string, std::unique_ptr<Resident>> residents;
    std::map<std::string, QualifiedProject> qualified;
    std::map<std::string, Clock::time_point> reader_requests;
    std::uint64_t resident_evictions = 0, resident_warm_requests = 0, resident_memory_evictions = 0;
    bool resident_memory_limited = false;
    Clock::time_point next_memory_check{};

    void trim_resident_memory() {
        const auto now = Clock::now();
        if (now < next_memory_check) return;
        next_memory_check = now + std::chrono::seconds(1);
        std::vector<std::string> roots;
        std::vector<ResidentRetentionSample> samples;
        for (const auto& [root, reader] : residents) {
            const bool pending = std::any_of(connections.begin(), connections.end(), [&](const auto& client) {
                return client->warm_pending && client->root.string() == root;
            });
            roots.push_back(root);
            samples.push_back({reader->process.rss_bytes(), reader->ready && reader->request_id.empty() && !pending,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - reader->last_used).count())});
        }
        while (const auto victim = resident_memory_victim(samples)) {
            residents.erase(roots[*victim]);
            samples[*victim] = {0, false, 0};
            ++resident_evictions;
            ++resident_memory_evictions;
            resident_memory_limited = true;
        }
    }
    std::map<std::string, std::string> resident_errors;
    std::map<std::string, Json> project_blockers;
    LocalWorkerProcess* refresh_worker = nullptr;
};
} // namespace

fs::path local_service_directory(const fs::path& input) {
    const auto home = fs::absolute(input).lexically_normal();
    if (fs::canonical(home) != home) throw Error("unsafe-home", "Service home must have no symlink components.");
    (void)directory_identity(home, true);
    return home / "service";
}
void service_write(ServiceChannel& channel, const std::string& bytes, ServiceDeadline deadline, const ServiceCancelled& cancelled) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        require_not_cancelled(cancelled);
        if (Clock::now() >= deadline) throw Error("service-timeout", "Local send deadline expired.");
        const auto count = channel.write(std::string_view(bytes).substr(sent));
        if (count) {
            if (!*count) throw Error("service-disconnected", "Local write made no progress.");
            sent += *count;
        } else channel.wait_write(std::min(deadline, Clock::now() + std::chrono::milliseconds(20)));
    }
}
std::string service_read(ServiceChannel& channel, std::size_t limit, ServiceDeadline deadline, const ServiceCancelled& cancelled) {
    std::string result;
    for (;;) {
        require_not_cancelled(cancelled);
        if (Clock::now() >= deadline) throw Error("service-timeout", "Local read deadline expired.");
        const auto bytes = channel.read(std::min(deadline, Clock::now() + std::chrono::milliseconds(20)));
        if (!bytes) continue;
        if (bytes->empty()) throw Error("service-disconnected", "Local reply ended before its frame.");
        result += *bytes;
        if (result.size() > limit) throw Error("service-frame-too-large", "Local frame exceeds its byte limit.");
        const auto end = result.find('\n');
        if (end == std::string::npos) continue;
        if (end + 1 != result.size()) throw Error("service-frame-invalid", "Only one frame is allowed.");
        result.pop_back(); return result;
    }
}

Json serve_local_projects(const LocalProjectRequest& request, const std::vector<fs::path>& roots,
                          const fs::path& executable, const std::function<bool()>& stopped, bool registered, bool supervised,
                          const std::function<void()>& started, const Json& usage_sources) {
    const auto reaping = service_reaping();
    if (roots.size() > kLocalServiceProjectLimit || (registered && registered_local_projects(request.home).size() > kLocalServiceProjectLimit))
        throw Error("registry-capacity", "This local service supports at most sixteen enrolled projects.");
    if (registered && !roots.empty())
        throw Error("registry-unavailable", "Registered service does not accept explicit roots.");
    check_local_service_supervision(request, executable, supervised);
    Service service(request, roots, executable, registered, supervised);
    if (started) started();
    std::jthread storage_maintenance([home = request.home](std::stop_token stop) {
        while (!stop.stop_requested()) {
            automatic_local_storage(home);
            for (int second = 0; second < 3000 && !stop.stop_requested(); ++second)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    std::jthread usage_collection;
#ifndef _WIN32
    if (registered && !usage_sources.is_null())
        usage_collection = std::jthread([&request, &usage_sources](std::stop_token stop) {
            try { watch_registered_model_usage(request.home, usage_sources, stop); }
            catch (...) { return; } // A stopped collector leaves a visibly stale snapshot, never a crashed service.
        });
#else
    (void)usage_sources;
#endif
    LocalEnrollmentHooks enrollment;
    enrollment.removed = [&](const auto& root) { service.retire_project(root); };
    enrollment.preparing = [&](const auto& root, bool active) {
        if (active) service.preparing_projects.insert(root);
        else service.preparing_projects.erase(root);
    };
    if (registered) {
        enrollment.read = [&] { service.retire_detached_errors(); return registered_local_projects(request.home); };
        enrollment.report = [&](const auto& project, const auto& error) { service.enrollment_result(project, error); };
    }
    auto result = watch_local_projects(request, roots, 0, [&] { return stopped() || service.stopping; },
                                      [&] { service.poll(); }, enrollment,
                                      [&](const auto& root, const auto& state) { service.project_state(root, state); },
                                      [&](const auto& project) { return service.refresh(project, stopped); },
                                      [&] { return service.reader_demand(); });
    service.drain_stop_replies();
    result["command"] = "serve";
    result["service_stopped"] = true;
    result["undelivered_stop_replies"] = service.undelivered_stop_replies;
    result["service_protocol"] = "shin.local-rpc.v1";
    if (result.contains("scheduler")) result["scheduler"]["query_transport"] = service_transport();
    return result;
}

std::unique_ptr<StateStore> lock_local_service_control(const fs::path& home) {
    const auto start_directory = fs::absolute(home).lexically_normal() / "service-start";
    ensure_private_directory(start_directory);
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    std::unique_ptr<StateStore> starter;
    while (!starter) {
        try { starter = std::make_unique<StateStore>(start_directory, true); }
        catch (const Error& error) {
            if (error.code != "writer-busy" || Clock::now() >= deadline) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    return starter;
}

Json start_local_service(const LocalProjectRequest& input, const fs::path& executable) {
    // An OS-managed child bootstraps its own registry and startup lock. Do not
    // hold that lock while waiting for it, or first-run startup deadlocks.
    const auto managed = start_installed_local_service(input, executable);
    const auto projects = registered_local_projects(input.home);
    if (projects.size() > kLocalServiceProjectLimit) throw Error("registry-capacity", "This local service supports at most sixteen enrolled projects.");
    auto control = input;
    control.home = fs::absolute(input.home).lexically_normal();
    control.command = "service-status";
    const auto admitted_start = [&](Json status) {
        if (input.command != "service-start") return status;
        const bool started = status.at("started");
        // Socket availability precedes registry admission. An explicit restart
        // must settle enrollment before a caller's first project request.
        const auto deadline = Clock::now() + std::chrono::seconds(60);
        while (!std::all_of(projects.begin(), projects.end(), [&](const auto& project) {
            return resident_ready(status, project.root) || status.at("enrollment_errors").contains(project.root.string()) ||
                ((projects.size() > resident_limit || status.value("resident_memory_limited", false)) &&
                    std::any_of(status.at("qualified_projects").begin(), status.at("qualified_projects").end(),
                    [&](const Json& entry) { return entry.at("root") == project.root.string(); }));
        })) {
            if (Clock::now() >= deadline)
                throw Error("service-enrollment-timeout", "Registered project preparation remains pending; the service stays available for diagnostics.");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            status = request_local_service(control, [&] { return Clock::now() >= deadline; });
        }
        status["started"] = started;
        status.erase("telemetry"); // Startup acknowledgement is not a dashboard/history response.
        return status;
    };
    if (managed) return admitted_start(*managed);
    const auto starter = lock_local_service_control(control.home);
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    const auto probe = [&] { return request_local_service(control, [&] { return Clock::now() >= deadline; }); };
    const auto require_registry = [](const Json& result) {
        if (result.value("enrollment_mode", "") != "registry")
            throw Error("service-mode-conflict", "An explicit-root service is running; stop it deliberately before registered startup.");
    };
    const auto compatible = [&](Json result, bool started) {
        require_registry(result);
        result["started"] = started;
        return admitted_start(std::move(result));
    };
    try { return compatible(probe(), false); }
    catch (const Error& error) {
        if (error.code != "service-unavailable" && error.code != "service-socket-unsafe" && error.code != "unsafe-directory") throw;
        // A missing endpoint is normal. A live service lock or any unsafe path
        // must not cause a second instance or silent endpoint replacement.
        const auto directory = local_service_directory(control.home);
        if (fs::exists(directory)) {
            StateStore owner(directory, false);
            if (owner.writer_present()) throw Error("service-unavailable", "The existing service owns its lock but is not responding.");
            check_stale_service_endpoint(control.home);
        }
    }
    const auto reaping = service_reaping();
    auto child = launch_service(fs::canonical(executable), control.home);
    while (Clock::now() < deadline) {
        if (child->exited())
            throw Error("service-start-failed", "Native service exited during startup; review registry and project bindings.");
        try {
            auto status = probe();
            require_registry(status);
            child->release();
            return compatible(std::move(status), true);
        } catch (const Error& error) {
            if (error.code != "service-unavailable" && error.code != "service-socket-unsafe" && error.code != "unsafe-directory") throw;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw Error("service-start-timeout", "Native service did not become available within its startup deadline.");
}

Json initialize_local_service(const LocalProjectRequest& input, const fs::path& executable) {
    const auto retry_until = Clock::now() + std::chrono::seconds(5);
    Json result;
    for (;;) {
        try { result = local_project(input); break; }
        catch (const Error& error) {
            if (error.code != "writer-busy" || Clock::now() >= retry_until) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    if (result.at("ok") != true) return result;
    auto request = input;
    request.root = result.at("root").get<std::string>();
    try {
        const auto workspace = request.home / "projects" / digest(request.root.string());
        const auto lifecycle = require_local_project_active(workspace);
        enroll_local_project(request);
        auto status = start_local_service(request, executable);
        result["daemon_started"] = status.at("started");
        request.command = "service-status";
        const auto deadline = Clock::now() + std::chrono::seconds(60);
        for (;;) {
            result["service"] = status;
            // Initialization reports admission, not the growing dashboard history.
            // Keep telemetry on service-status so a successful init stays bounded.
            result["service"].erase("telemetry");
            if (resident_ready(status, request.root)) {
                result["service_enrolled"] = true;
                if (result.value("binding_kind", "") != "existing-route") result["refresh_management"] = "local-service";
                return result;
            }
            if (status.at("enrollment_errors").contains(request.root.string())) {
                const auto code = status.at("enrollment_errors").at(request.root.string()).get<std::string>();
                // Registry polling may still report the previous detached phase.
                // Wait only for this unchanged explicit activation; readiness still
                // requires a fresh service admission, never this local observation.
                if (code != "project-detached" || local_project_lifecycle(workspace) != lifecycle)
                    throw Error("service-enrollment-failed", "Service rejected project enrollment: " + code);
            }
            if (Clock::now() >= deadline) throw Error("service-enrollment-timeout", "Project enrollment remains pending; inspect service-status.");
            if (std::find(status.at("projects").begin(), status.at("projects").end(), Json(request.root.string())) != status.at("projects").end()) {
                auto wanted = request;
                wanted.command = "status";
                try { (void)request_local_service(wanted, [&] { return Clock::now() >= deadline; }); }
                catch (const Error& error) {
                    if (error.code != "project-updating" && error.code != "project-busy" && error.code != "service-busy") throw;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            status = request_local_service(request, [&] { return Clock::now() >= deadline; });
        }
    } catch (const Error& error) {
        result["ok"] = false;
        result["service_enrolled"] = false;
        result["service"]["error"] = {{"code", error.code}, {"message", error.what()}};
        result["next_action"] = "review-service-startup";
        return result;
    }
}

Json detach_local_service(const LocalProjectRequest& input) {
    auto request = input;
    request.root = fs::canonical(input.root);
    request.home = fs::absolute(input.home).lexically_normal();
    const auto workspace = request.home / "projects" / digest(request.root.string());
    // Detach never creates a home or binding and never stops the shared service.
    (void)directory_identity(workspace, true);
    request.command = "_detach";
    const auto deadline = Clock::now() + std::chrono::seconds(65);
    for (;;) {
        try { (void)local_project(request); break; }
        catch (const Error& error) {
            if (error.code != "writer-busy" || Clock::now() >= deadline) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    for (;;) {
        if (Clock::now() >= deadline)
            throw Error("detach-pending", "Project is fenced; retry detach to confirm controller shutdown. Data is preserved.");
        auto control = request;
        control.command = "service-status";
        bool retired = false;
        try {
            const auto status = request_local_service(control);
            retired = std::none_of(status.at("projects").begin(), status.at("projects").end(),
                [&](const auto& root) { return root == request.root.string(); }) &&
                std::none_of(status.at("preparing_projects").begin(), status.at("preparing_projects").end(),
                [&](const auto& root) { return root == request.root.string(); });
        } catch (const Error& error) {
            if (error.code != "service-unavailable" && error.code != "unsafe-directory") throw;
            const auto directory = local_service_directory(request.home);
            retired = !fs::exists(fs::symlink_status(directory)) || !StateStore(directory, false).writer_present();
        }
        if (retired) {
            request.command = "_detach-complete";
            try { return local_project(request); }
            catch (const Error& error) {
                if (error.code != "writer-busy" && error.code != "project-busy") throw;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

namespace {
Json exchange_local_request(const LocalProjectRequest& request, const std::function<bool()>& cancelled,
                            const std::string& observer_version, const std::string& core_version) {
    const auto started = Clock::now();
    require_not_cancelled(cancelled);
    const auto deadline = Clock::now() + std::chrono::seconds(35);
    auto channel = connect_service(request.home, deadline, cancelled);
    const auto id = digest(std::to_string(service_process_id()) + ":" + std::to_string(Clock::now().time_since_epoch().count()));
    auto input = reply(id);
    const bool control = request.command == "service-status" || request.command == "service-stop";
    if (!control && (observer_version != SHIN_OBSERVER_VERSION || core_version != SHIN_VERSION))
        throw Error("service-method-denied", "Version-pinned installation traffic cannot read project context.");
    input["observer_version"] = observer_version;
    input["core_version"] = core_version;
    input.update({{"command", request.command}, {"root", control ? "" : fs::canonical(request.root).string()},
        {"query", request.query}, {"path", request.path}, {"record", request.record}, {"snapshot", request.snapshot}});
    if (request.diagnostic) input["diagnostic"] = true;
    if (!request.session_id.empty()) input["session_id"] = request.session_id;
    if (!request.model_id.empty()) input["model_id"] = request.model_id;
    if (!request.parent_request_id.empty()) input["parent_request_id"] = request.parent_request_id;
    const auto frame = input.dump() + '\n';
    if (frame.size() > request_limit) throw Error("service-frame-too-large", "Local request exceeds its byte limit.");
    service_write(*channel, frame, deadline, cancelled);
    const auto response = parse_json(service_read(*channel, response_limit, deadline, cancelled));
    require_not_cancelled(cancelled);
    auto result = control ? local_rpc::control_response(response, id, observer_version, core_version) : project_response(response, id);
    if (!control) result["request_trace"] = {{"request_id", digest(id)},
        {"session_correlated", !request.session_id.empty()}, {"model_id", request.model_id},
        {"model_attribution", request.model_id.empty() ? "unknown" : "caller-reported"}, {"task_outcome", "not-observed"}};
    result["transport"] = service_transport();
    result["transport_duration_ms"] = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    if (request.command == "service-status") bound_local_service_status(result);
    return result;
}
} // namespace
Json request_local_service(const LocalProjectRequest& request, const std::function<bool()>& cancelled) {
    return exchange_local_request(request, cancelled, SHIN_OBSERVER_VERSION, SHIN_VERSION);
}
Json control_local_service_for_update(const fs::path& home, const std::string& observer_version, bool stop, const std::string& core_version) {
    LocalProjectRequest request; request.home = home; request.command = stop ? "service-stop" : "service-status";
    return exchange_local_request(request, {}, observer_version, core_version);
}
} // namespace shin::observer
