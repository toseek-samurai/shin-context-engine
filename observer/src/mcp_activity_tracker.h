#pragma once
#include "shin/observer/local_mcp_activity.h"
#include "shin/observer/local_project.h"
#include "shin/observer/local_service_platform.h"
#include <mcp/JsonRpc.hpp>
#include <chrono>
#include <algorithm>
#include <map>
#include <mutex>

namespace shin::observer {
// SDK filters observe tools/call even when validation prevents its handler running.
class McpActivityTracker {
    using Clock = std::chrono::steady_clock;
public:
    struct Call { Json row; Clock::time_point started; };
    explicit McpActivityTracker(LocalMcpActivity& input) : journal(input),
        instance(digest(std::to_string(service_process_id()) + ":" + std::to_string(Clock::now().time_since_epoch().count()))) {}
    std::optional<Call> begin(const mcp::JsonRpcMessage& message) {
        const auto* request = mcp::AsRequest(message);
        if (!request || request->method != "tools/call") return {};
        std::string method = "unknown";
        if (request->params && request->params->IsObject()) {
            const auto* name = request->params->Find("name");
            if (name && name->IsString()) for (const auto* allowed : {"query", "inspect", "read", "status", "projects", "help", "checkpoint", "resume"})
                if (name->GetString() == std::string("shin_") + allowed) method = allowed;
        }
        std::lock_guard lock(mutex);
        Call call{{{"call_id", digest(instance + ":" + std::to_string(++sequence))}, {"at_ms", now_ms()},
            {"method", method}, {"outcome", "unfinished"}, {"write_state", "unobserved"}}, Clock::now()};
        journal.append(call.row); return call;
    }
    void activate(const mcp::JsonRpcMessage& message, std::optional<Call> call) {
        if (!call) return;
        const std::lock_guard lock(mutex);
        active.emplace(key(mcp::AsRequest(message)->id), std::move(*call));
    }
    void attribute(const mcp::RequestId& id, const LocalProjectRequest& request) {
        const std::lock_guard lock(mutex);
        const auto it = active.find(key(id)); if (it == active.end()) return;
        auto& row = it->second.row;
        row["project_id"] = request.root.empty() ? "" : digest(request.root.string());
        row["session_id"] = request.session_id; row["model_id"] = request.model_id;
        row["task_id"] = request.task_id; row["intent"] = request.intent;
        row["parent_request_id"] = request.parent_request_id;
        row["purpose"] = request.diagnostic ? "diagnostic" : "unclassified";
    }
    void service_request(const mcp::RequestId& id, const std::string& request_id) {
        const std::lock_guard lock(mutex);
        const auto it = active.find(key(id));
        if (it != active.end()) it->second.row["service_request_id"] = request_id;
    }
    void result(const mcp::RequestId& id, const Json& result) {
        const std::lock_guard lock(mutex);
        const auto it = active.find(key(id)); if (it == active.end()) return;
        it->second.row["outcome"] = result.value("ok", false) ? "success" : "tool-error";
        if (result.value("snapshot_id", Json()).is_string())
            it->second.row["generation_id"] = digest(result.at("snapshot_id").get<std::string>());
        const auto work = result.value("work_performed", Json::object());
        if (it->second.row.at("method") == "read" && result.value("ok",false) && work.is_object()) {
            const auto identity = work.value("read_identity",Json::object());
            const auto opaque = [](const Json& value) {
                return value.is_string() && value.get_ref<const std::string&>().size() == 64 &&
                    value.get_ref<const std::string&>().find_first_not_of("0123456789abcdef") == std::string::npos;
            };
            if (identity.is_object() && identity.size() == 2 && opaque(identity.value("file_id",Json())) &&
                opaque(identity.value("version_id",Json()))) {
                it->second.row["read_file_id"] = identity.at("file_id");
                it->second.row["read_version_id"] = identity.at("version_id");
            }
        }
        if (!result.value("ok", false) && result.contains("error") && result.at("error").is_object()) {
            const auto code = result.at("error").value("code", Json());
            if (code.is_string() && code.get_ref<const std::string&>().size() <= 96 &&
                code.get_ref<const std::string&>().find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos)
                it->second.row["error_code"] = code;
        }
    }
    void finish(const mcp::JsonRpcMessage& message, bool written) {
        std::optional<mcp::RequestId> id;
        if (const auto* response = mcp::AsResponse(message)) id = response->id;
        if (const auto* error = mcp::AsError(message)) id = error->id;
        if (!id) return;
        const std::lock_guard lock(mutex);
        const auto it = active.find(key(*id)); if (it == active.end()) return;
        if (mcp::AsError(message)) {
            it->second.row["outcome"] = "rpc-error";
            it->second.row["error_code"] = "mcp-protocol-error";
        }
        complete(it->second, written); active.erase(it);
    }
    void rejected(std::optional<Call> call, bool duplicate, bool written) {
        if (!call) return;
        call->row["outcome"] = "rpc-error";
        call->row["error_code"] = duplicate ? "mcp-duplicate-id" : "mcp-adapter-busy";
        complete(*call, written);
    }
private:
    static std::string key(const mcp::RequestId& id) {
        if (const auto* number = std::get_if<std::int64_t>(&id)) return "n:" + std::to_string(*number);
        return "s:" + std::get<std::string>(id);
    }
    void complete(Call& call, bool written) {
        call.row["write_state"] = written ? "written" : "failed";
        call.row["duration_ms"] = std::min(60000.0, std::chrono::duration<double, std::milli>(Clock::now() - call.started).count());
        journal.append(call.row);
    }
    LocalMcpActivity& journal;
    std::string instance;
    std::mutex mutex;
    std::uint64_t sequence = 0;
    std::map<std::string, Call> active;
};
}
