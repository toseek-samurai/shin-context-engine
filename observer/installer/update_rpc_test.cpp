#include "shin/observer/local_rpc.h"
#include "shin/observer/local_update_diagnostics.h"
#include <iostream>
using namespace shin::observer;
namespace {
void check(bool value) { if (!value) throw std::runtime_error("RPC version boundary failed"); }
template<class F> void rejects(F fn, const std::string& code = "service-response-invalid") {
    try { fn(); } catch (const Error& e) { check(e.code == code); return; }
    throw std::runtime_error("RPC version boundary accepted an invalid response");
}
}
int main() {
    try {
        auto response = local_rpc::reply("update-control");
        response["observer_version"] = "2.0.5-dev.7";
        response["ok"] = true;
        response["result"] = {{"ok", true}};
        rejects([&] { local_rpc::project_response(response, "update-control"); });
        check(local_rpc::control_response(response, "update-control", "2.0.5-dev.7").at("ok"));
        rejects([&] { local_rpc::control_response(response, "different-request", "2.0.5-dev.7"); });
        rejects([&] { local_rpc::control_response(response, "update-control", "2.0.5-dev.6"); });
        response["core_version"] = "2.0.5-dev.1";
        rejects([&] { local_rpc::project_response(response, "update-control"); });
        check(local_rpc::control_response(response, "update-control", "2.0.5-dev.7", "2.0.5-dev.1").at("ok"));
        rejects([&] { local_rpc::control_response(response, "update-control", "2.0.5-dev.7", "wrong-core"); });
        response["core_version"] = "incompatible-core";
        rejects([&] { local_rpc::control_response(response, "update-control", "2.0.5-dev.7"); });
        response = local_rpc::failure("update-control", "writer-busy", "Owned service is busy.");
        response["observer_version"] = "2.0.5-dev.7";
        rejects([&] { local_rpc::control_response(response, "update-control", "2.0.5-dev.7"); }, "writer-busy");
        // Enrollment diagnostics retain bounded codes, never paths/messages.
        const auto diagnostic = update_service_diagnostic({{"registry_error", "registry-busy"},
            {"enrollment_errors", {{"/private/consumer-one", "project-busy"},
                {"/private/consumer-two", "refresh-timeout"}, {"/private/consumer-three", "binding-changed"},
                {"/private/consumer-four", "Bearer sensitive value must be omitted"}}}}, "activation");
        check(diagnostic.dump().size() <= 1024 && diagnostic.at("truncated"));
        check(diagnostic.dump().find("/private/") == std::string::npos);
        check(diagnostic.dump().find("sensitive") == std::string::npos);
        check(diagnostic.dump().find("project-busy") != std::string::npos);
        check(transient_update_project_error("project-busy") && transient_update_project_error("refresh-timeout"));
        check(!transient_update_project_error("binding-identity-changed") && !transient_update_project_error("index-integrity-failed"));
        const auto merged = merge_update_diagnostic(diagnostic,
            update_service_diagnostic({{"registry_error", "installation-recovery-command"}}, "rollback"));
        check(merged.dump().find("installation-recovery-command") != std::string::npos);
        check(merged.dump().size() <= 1024);
        std::cout << "Version-pinned update control preserves normal RPC and Core checks.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
