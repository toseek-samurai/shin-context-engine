#include "shin/observer/local_rpc.h"
#include "../src/local_sessions.h"
#include <iostream>
using namespace shin::observer;
namespace {
void check(bool value) { if (!value) throw std::runtime_error("RPC regression"); }
template<class F> void refused(const char* code, F action) {
    try { action(); } catch (const Error& error) { check(error.code == code); return; }
    throw std::runtime_error("unexpected RPC acceptance");
}
}
int main() try {
    Json session{{"continuation_ready",false},{"review_required",true},{"next_actions",Json::array({
        {{"code","rebuild-index"},{"next_command",{{"index_mutation",true},{"argv",Json::array({"shin-context","build"})}}}},
        {{"code","another-mutating-action"},{"next_command",{{"index_mutation",true}}}},
        {{"code","inspect-impact"},{"next_command",{{"index_mutation",false},{"argv",Json::array({"shin-context","impact"})}}}}
    })}};
    const auto readonly_action = session.at("next_actions").at(2);
    local_session_actions(session);
    auto request = local_rpc::reply("query-wait");
    request.update({{"command","query"},{"root","/fixture"},{"query","needle"},{"path",""},{"record",""},{"snapshot",""}});
    for (const auto budget : {5000, 20000}) {
        request["readiness_wait_ms"] = budget;
        local_rpc::validate_request(request);
    }
    for (const auto& budget : Json::array({4999,20001,-1,true,20000.0,"20000",nullptr})) {
        request["readiness_wait_ms"] = budget;
        refused("service-request-invalid", [&] { local_rpc::validate_request(request); });
    }
    request["readiness_wait_ms"] = 20000; request["command"] = "status";
    refused("service-request-invalid", [&] { local_rpc::validate_request(request); });
    for (const auto i : {0,1}) {
        check(session.at("next_actions").at(i).at("code") == "wait-for-background-qualification");
        check(!session.at("next_actions").at(i).contains("next_command"));
        check(session.at("next_actions").at(i).at("automatic_execution") == false);
    }
    check(session.at("next_actions").at(2) == readonly_action && session.at("review_required") == true);
    auto success = local_rpc::reply("id"); success.update({{"ok", true}, {"result", {{"ok", true}}}});
    check(local_rpc::project_response(success, "id").at("ok") == true);
    auto mismatch = local_rpc::failure("id", "service-version-mismatch", "test");
    mismatch["observer_version"] = "2.0.5-dev.999";
    refused("service-version-mismatch", [&] { local_rpc::project_response(mismatch, "id"); });
    refused("service-response-invalid", [&] { local_rpc::control_response(mismatch, "id", SHIN_OBSERVER_VERSION); });
    refused("service-response-invalid", [&] { local_rpc::project_response(mismatch, "wrong"); });
    for (const auto& change : Json::array({
            {{"ok", true}}, {{"result", {{"ok", true}}}}, {{"request_id", 5}},
            {{"observer_version", Json::array()}}, {{"schema_version", "other"}},
            {{"error", {{"code", "service-version-mismatch"}, {"message", 5}}}},
            {{"error", {{"code", "service-unavailable"}, {"message", "test"}}}}})) {
        auto value = mismatch; value.update(change);
        refused("service-response-invalid", [&] { local_rpc::project_response(value, "id"); });
    }
    success["observer_version"] = "2.0.5-dev.999";
    refused("service-response-invalid", [&] { local_rpc::project_response(success, "id"); });
    auto same_version = local_rpc::failure("id", "service-busy", "test");
    refused("service-busy", [&] { local_rpc::project_response(same_version, "id"); });
    std::cout << "mismatch diagnostics preserve response identity and installer version fences\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
