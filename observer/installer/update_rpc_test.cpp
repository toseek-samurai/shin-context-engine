#include "shin/observer/local_rpc.h"
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
        std::cout << "Version-pinned update control preserves normal RPC and Core checks.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
