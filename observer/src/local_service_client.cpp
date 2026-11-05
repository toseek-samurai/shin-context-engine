#include "shin/observer/local_service.h"
#include "shin/observer/local_mcp_bridge.h"
#include "shin/observer/local_service_platform.h"
#include "shin/observer/local_rpc.h"
#include "shin/observer/local_telemetry.h"
#include "local_sessions.h"
#include <chrono>

namespace shin::observer {
using Clock = std::chrono::steady_clock;
using local_rpc::reply;
using local_rpc::project_response;
namespace { constexpr std::size_t request_limit = 8192, response_limit = 128 * 1024; }
namespace {
void require_not_cancelled(const ServiceCancelled& cancelled) {
    if (cancelled && cancelled()) throw Error("service-request-cancelled", "Local request was cancelled.");
}
Json exchange_local_request(const LocalProjectRequest& request, const std::function<bool()>& cancelled,
                            const std::string& observer_version, const std::string& core_version,
                            const VerifiedMcpRuntime* mcp_runtime = nullptr) {
    const auto started = Clock::now();
    require_not_cancelled(cancelled);
    const auto deadline = Clock::now() + std::chrono::seconds(35);
    auto channel = connect_service(request.home, deadline, cancelled);
    const auto id = digest(std::to_string(service_process_id()) + ":" + std::to_string(Clock::now().time_since_epoch().count()));
    if (request.observe_request_id) request.observe_request_id(digest(id));
    auto input = reply(id);
    const bool control = request.command == "service-status" || request.command == "service-stop";
    if (!control && !mcp_runtime && (observer_version != SHIN_OBSERVER_VERSION || core_version != SHIN_VERSION))
        throw Error("service-method-denied", "Version-pinned installation traffic cannot read project context.");
    input["observer_version"] = observer_version;
    input["core_version"] = core_version;
    if (mcp_runtime) {
        if (control) throw Error("service-method-denied", "MCP runtime handoff cannot control the service.");
        input["mcp_runtime"] = mcp_runtime->binding();
    }
    input.update({{"command", request.command}, {"root", control ? "" : fs::canonical(request.root).string()},
        {"query", request.query}, {"path", request.path}, {"record", request.record}, {"snapshot", request.snapshot}});
    if (request.diagnostic) input["diagnostic"] = true;
    if (request.command == "status" && !request.wait_for_ready) input["wait_for_ready"] = false;
    if (request.readiness_wait_ms) input["readiness_wait_ms"] = request.readiness_wait_ms;
    if (!request.session_id.empty()) input["session_id"] = request.session_id;
    if (!request.model_id.empty()) input["model_id"] = request.model_id;
    if (!request.parent_request_id.empty()) input["parent_request_id"] = request.parent_request_id;
    if (request.command == "checkpoint" || request.command == "resume") input["session"] = local_session_payload(request);
    const auto frame = input.dump() + '\n';
    if (frame.size() > request_limit) throw Error("service-frame-too-large", "Local request exceeds its byte limit.");
    service_write(*channel, frame, deadline, cancelled);
    const auto response = parse_json(service_read(*channel, response_limit, deadline, cancelled));
    require_not_cancelled(cancelled);
    auto result = mcp_runtime ? local_rpc::mcp_response(response,id,*mcp_runtime) :
        control ? local_rpc::control_response(response, id, observer_version, core_version) : project_response(response, id);
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
Json request_verified_mcp_service(const LocalProjectRequest& request, const VerifiedMcpRuntime& runtime,
                                 const std::function<bool()>& cancelled) {
    return exchange_local_request(request,cancelled,runtime.observer_version(),runtime.core_version(),&runtime);
}
Json control_local_service_for_update(const fs::path& home, const std::string& observer_version, bool stop, const std::string& core_version) {
    LocalProjectRequest request; request.home = home; request.command = stop ? "service-stop" : "service-status";
    return exchange_local_request(request, {}, observer_version, core_version);
}
} // namespace shin::observer
