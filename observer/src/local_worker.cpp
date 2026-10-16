#include "shin/observer/local_worker.h"
#include "shin/observer/local_worker_process.h"
#include "shin/observer/local_service.h"
#include "shin/observer/local_service_platform.h"
#include "shin/observer/local_rpc.h"
#include "shin/observer/local_query.h"
#include "context_internal.h"
#include <chrono>
#include <atomic>
#include <thread>
#include <set>

namespace shin::observer {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t response_limit = 128 * 1024;
void require_binding(const LocalProjectRequest& request, bool workspace_required = true) {
    if (!request.root.is_absolute() || !request.home.is_absolute() ||
        request.maintenance_root_identity.empty() || (workspace_required && request.maintenance_workspace_identity.empty()))
        throw Error("service-request-invalid", "Worker requires absolute roots and explicit directory identities.");
}
}
Json request_local_worker(const LocalProjectRequest& request, const fs::path& executable,
                          const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) throw Error("service-request-cancelled", "Local request was cancelled.");
    const bool refresh = request.command == "_refresh";
    if (!refresh && request.command != "status" && request.command != "query" && request.command != "inspect" && request.command != "read")
        throw Error("service-method-denied", "Worker accepts project reads or internal refresh only.");
    const auto home = fs::canonical(request.home), root = fs::canonical(request.root);
    (void)directory_identity(home, true);
    const auto root_id = request.maintenance_root_identity.empty() ? directory_identity(root) : request.maintenance_root_identity;
    const auto workspace_id = request.maintenance_workspace_identity.empty() ?
        directory_identity(home / "projects" / digest(root.string()), true) : request.maintenance_workspace_identity;
    static std::atomic<std::uint64_t> serial{0};
    const auto id = digest(std::to_string(service_process_id()) + ":" + std::to_string(++serial) + ":" +
        std::to_string(Clock::now().time_since_epoch().count()));
    Json payload;
    if (refresh) payload = {{"schema_version", "shin.local-refresh.v1"}, {"version", SHIN_OBSERVER_VERSION},
        {"core_version", SHIN_VERSION}, {"root", root.string()}, {"home", home.string()},
        {"root_identity", root_id}, {"workspace_identity", workspace_id}, {"worktree", request.maintenance_worktree}};
    else {
        payload = local_rpc::reply(id);
        payload.update({{"command", request.command}, {"root", root.string()}, {"home", home.string()},
            {"root_identity", root_id}, {"workspace_identity", workspace_id}, {"query", request.query},
            {"path", request.path}, {"record", request.record}, {"snapshot", request.snapshot}});
    }
    if (refresh && request.maintenance_progress) payload["diagnostics"] = true;
    const auto reaping = service_reaping();
    LocalWorkerProcess child(executable, refresh ? LocalWorkerRole::refresh : LocalWorkerRole::read);
    child.send(payload, Clock::now() + std::chrono::seconds(2), cancelled);
    std::function<void(const Json&)> progress;
    if (refresh && request.maintenance_progress) progress = [&](const Json& frame) {
            const auto stage = frame.value("stage", "");
            const std::set<std::string> allowed{"admission", "observation", "candidate-build", "candidate-validation",
                "publication", "qualification", "source-cache", "candidate-source-cache", "source-cache-transfer", "final-verification"};
            if (allowed.contains(stage)) try { request.maintenance_progress(stage); } catch (...) { return; }
        };
    const auto response = child.receive(Clock::now() + std::chrono::seconds(refresh ? 60 : 35), cancelled, progress);
    child.stop();
    if (!refresh) return local_rpc::project_response(response, id);
    if (!response.is_object() || !response.contains("ok") || !response.at("ok").is_boolean())
        throw Error("refresh-worker-failed", "Invalid refresh worker response.");
    if (response.contains("error"))
        throw Error(response.at("error").at("code"), response.at("error").at("message"), response.at("error").value("diagnostic", Json()));
    return response;
}
Json qualify_local_project(const LocalProjectRequest& input, const fs::path& executable,
                          const std::function<bool()>& cancelled) {
    auto request = input; request.command = "status";
    request.query.clear(); request.path.clear(); request.record.clear(); request.snapshot.clear();
    return request_local_worker(request, executable, cancelled);
}
Json execute_local_worker_read(const Json& payload) {
    std::string id;
    Json response;
    try {
        auto input = payload;
        for (const auto* field : {"home", "root_identity", "workspace_identity"}) input.erase(field);
        local_rpc::validate_request(input);
        id = input.at("request_id").get<std::string>();
        LocalProjectRequest request;
        request.command = input.at("command");
        if (!std::set<std::string>{"status", "query", "inspect", "read"}.contains(request.command))
            throw Error("service-method-denied", "Worker accepts only project reads.");
        request.root = input.at("root").get<std::string>();
        request.home = local_rpc::string_field(payload, "home", 4096);
        request.maintenance_root_identity = local_rpc::string_field(payload, "root_identity", 128);
        request.maintenance_workspace_identity = local_rpc::string_field(payload, "workspace_identity", 128);
        require_binding(request);
        request.query = input.at("query"); request.path = input.at("path");
        request.record = input.at("record"); request.snapshot = input.at("snapshot");
        response = local_rpc::reply(id);
        const auto retry_until = Clock::now() + std::chrono::seconds(2);
        for (;;) {
            try { response["result"] = local_project(request); break; }
            catch (const Error& error) {
                if ((error.code != "project-busy" && error.code != "binding-changed") || Clock::now() >= retry_until) throw;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        response["ok"] = response.at("result").at("ok");
    } catch (const Error& error) { response = local_rpc::failure(id, error.code, error.what(), error.diagnostic); }
    catch (const shin::context::CliError& error) { response = local_rpc::failure(id, error.code(), error.what()); }
    catch (const std::exception&) { response = local_rpc::failure(id, "service-request-failed", "Native project request failed."); }
    if (response.dump().size() + 1 > response_limit)
        response = local_rpc::failure(id, "service-frame-too-large", "Native response exceeds its byte limit.");
    return response;
}
Json execute_local_worker_refresh(const Json& payload, const std::function<bool()>& stopped,
                                  const std::function<void(const Json&)>& progress) {
    Json result;
    try {
        if (!payload.is_object() || (payload.size() != 8 && payload.size() != 9) ||
            payload.value("schema_version", "") != "shin.local-refresh.v1" ||
            payload.value("version", "") != SHIN_OBSERVER_VERSION || payload.value("core_version", "") != SHIN_VERSION)
            throw Error("refresh-worker-failed", "Invalid refresh request identity.");
        const bool diagnostics = payload.value("diagnostics", false);
        if (payload.size() == 9 && (!payload.contains("diagnostics") || !payload.at("diagnostics").is_boolean()))
            throw Error("refresh-worker-failed", "Invalid refresh diagnostics option.");
        LocalProjectRequest request;
        request.command = "_refresh";
        request.root = local_rpc::string_field(payload, "root", 4096);
        request.home = local_rpc::string_field(payload, "home", 4096);
        request.maintenance_root_identity = local_rpc::string_field(payload, "root_identity", 128);
        request.maintenance_workspace_identity = local_rpc::string_field(payload, "workspace_identity", 128);
        // First controller admission still verifies the persisted local binding;
        // its workspace identity is returned by that admission.
        require_binding(request, false);
        request.maintenance_worktree = local_rpc::string_field(payload, "worktree", 128);
        request.maintenance_stop = stopped;
        const auto started = Clock::now();
        const std::set<std::string> stages{"admission", "observation", "candidate-build", "candidate-validation",
            "publication", "qualification", "source-cache", "candidate-source-cache", "source-cache-transfer", "final-verification"};
        std::size_t progress_count = 0;
        if (diagnostics) request.maintenance_progress = [&, started](const std::string& stage) {
            if (!progress || !stages.contains(stage) || progress_count >= 32) return;
            ++progress_count;
            try {
                progress(Json{{"kind", "progress"}, {"stage", stage},
                    {"elapsed_ms", std::chrono::duration<double, std::milli>(Clock::now() - started).count()}});
            } catch (...) { return; /* Diagnostics cannot affect refresh authority. */ }
        };
        result = local_project(request);
    } catch (const Error& error) { result = {{"ok", false}, {"error", error_json(error)}}; }
    catch (const shin::context::CliError& error) { result = {{"ok", false}, {"error", {{"code", error.code()}, {"message", error.what()}}}}; }
    catch (const std::exception&) { result = {{"ok", false}, {"error", {{"code", "refresh-worker-failed"}, {"message", "Native refresh failed."}}}}; }
    if (result.dump().size() + 1 > response_limit)
        result = {{"ok", false}, {"error", {{"code", "refresh-worker-failed"}, {"message", "Refresh response exceeds its bound."}}}};
    return result;
}
Json local_worker_metric(const Json& response, double duration_ms) {
    const auto code = response.contains("error") ? response.at("error").value("code", "request-failed") :
        response.at("ok") == true ? "" : "result-not-ok";
    return {{"schema_version", "shin.worker-metric.v1"}, {"ok", response.at("ok")},
        {"duration_ms", duration_ms}, {"response_bytes", response.dump().size() + 1}, {"error_code", code}};
}
LocalResidentWorker::LocalResidentWorker(const Json& payload) {
    if (!payload.is_object() || payload.size() != 7 || payload.at("schema_version") != "shin.local-resident.v1" ||
        payload.at("version") != SHIN_OBSERVER_VERSION || payload.at("core_version") != SHIN_VERSION)
        throw Error("resident-frame-invalid", "Invalid resident preparation envelope.");
    request_.command = "status";
    request_.root = local_rpc::string_field(payload, "root", 4096);
    request_.home = local_rpc::string_field(payload, "home", 4096);
    request_.maintenance_root_identity = local_rpc::string_field(payload, "root_identity", 128);
    request_.maintenance_workspace_identity = local_rpc::string_field(payload, "workspace_identity", 128);
    require_binding(request_);
    request_.query_session = &session_;
    const auto retry_until = Clock::now() + std::chrono::seconds(2);
    for (;;) {
        try {
            const auto result = local_project(request_);
            if (result.at("ok") != true || !session_)
                throw Error("resident-not-qualified", "Project generation is not qualified for resident retrieval.");
            break;
        } catch (const Error& error) {
            if ((error.code != "project-busy" && error.code != "binding-changed") || Clock::now() >= retry_until) throw;
            session_.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    request_.query_session = nullptr;
    if (!observed_unchanged()) throw Error("project-updating", "Sources changed during preparation.");
}
bool LocalResidentWorker::observed_unchanged() const { return session_->observed_unchanged(); }
Json LocalResidentWorker::ready() const { return {{"kind", "ready"}, {"preparation", session_->description()}}; }
Json LocalResidentWorker::execute(const std::string& frame, Clock::time_point started) {
    std::string id;
    Json response;
    bool reusable = true;
    try {
        const auto payload = parse_json(frame);
        local_rpc::validate_request(payload);
        id = payload.at("request_id");
        if (payload.at("root") != request_.root.string()) throw Error("service-project-denied", "Resident root differs.");
        request_.command = payload.at("command");
        request_.query = payload.at("query");
        request_.path = payload.at("path");
        request_.record = payload.at("record");
        request_.snapshot = payload.at("snapshot");
        response = local_rpc::reply(id);
        response["result"] = session_->execute(request_);
        response["ok"] = response.at("result").at("ok");
    } catch (const Error& error) {
        response = local_rpc::failure(id, error.code, error.what());
        reusable = error.code == "source-snapshot-mismatch" || error.code == "service-method-denied";
    } catch (const shin::context::CliError& error) {
        response = local_rpc::failure(id, error.code(), error.what());
        reusable = error.code().starts_with("source-read-unknown") || error.code() == "invalid-inspect-path";
    } catch (const std::exception&) {
        response = local_rpc::failure(id, "service-request-failed", "Resident project request failed.");
        reusable = false;
    }
    if (response.dump().size() + 1 > response_limit)
        response = local_rpc::failure(id, "service-frame-too-large", "Resident response exceeds its byte limit.");
    auto metric = local_worker_metric(response, std::chrono::duration<double, std::milli>(Clock::now() - started).count());
    if (response.contains("result") && response.at("result").contains("phase_ms"))
        metric["phase_ms"] = response.at("result").at("phase_ms");
    return {{"kind", "response"}, {"response", response}, {"metric", metric}, {"reusable", reusable}};
}
}
