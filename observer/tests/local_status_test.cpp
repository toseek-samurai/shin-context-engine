#include "shin/observer/local_status.h"
#include <iostream>
#include <stdexcept>

using namespace shin::observer;
int main() {
    try {
        const auto require = [](bool condition) { if (!condition) throw std::runtime_error("status rendering assertion"); };
        Json status{{"command", "service-status"}, {"service_running", true}, {"supervision", "unmanaged"},
            {"query_workers", 0}, {"projects", Json::array({"/one", "/two", "/three"})},
            {"preparing_projects", Json::array({"/four"})},
            {"enrollment_errors", {{"/five", "binding-changed"}}},
            {"resident_errors", {{"/six", "reader-failed"}, {"/four", "project-updating"}}},
            {"registry_error", "registry-invalid"},
            {"telemetry", {{"uptime_ms", 2500}, {"samples", Json::array({{{"rss_bytes", 1048576}}})},
                {"request_metrics", {{"responses_ready", 3}, {"response_errors", 2}, {"unobserved", 1}}},
                {"project_states", Json::array({
                    {{"root", "/one"}, {"phase", "checking"}, {"query_ready", true}, {"failures", 7}, {"last_check_age_ms", 1200}},
                    {{"root", "/two"}, {"phase", "current"}, {"query_phase", "standby"}},
                    {{"root", "/three"}, {"phase", "checking"}, {"query_ready", true},
                        {"active_issue", true}, {"error_code", "refresh-validation-failed"}}
                })}}}};
        const auto before = status;
        const auto text = local_status_text(status);
        require(status == before);
        require(text.find("service: running") != std::string::npos);
        require(text.find("2.5 s") != std::string::npos && text.find("1.0 MiB") != std::string::npos);
        require(text.find("3 ready, 2 failed, 1 unobserved") != std::string::npos);
        require(text.find("Projects: 6") != std::string::npos);
        require(text.find("ready      checking") != std::string::npos);
        require(text.find("standby    current") != std::string::npos);
        require(text.find("blocked    checking") != std::string::npos);
        require(text.find("preparing  unknown") != std::string::npos);
        require(text.find("binding-changed") != std::string::npos && text.find("reader-failed") != std::string::npos);
        require(text.find("Issue: \"project-updating\"") == std::string::npos);
        require(text.find("Registry issue: \"registry-invalid\"") != std::string::npos);
        status["telemetry"]["samples"] = Json::array();
        status["telemetry"]["projection"]["truncated"] = true;
        const auto sparse = local_status_text(status);
        require(sparse.find("unknown MiB") != std::string::npos && sparse.find("Historical detail is truncated") != std::string::npos);
        require(local_status_text({{"command", "service-status"}, {"service_running", true}}).find("Projects: 0") != std::string::npos);
        require(local_status_text({{"command", "status"}, {"local_ready", false}}).find("Search: not ready") != std::string::npos);
        require(local_status_text({{"command", "status"}, {"local_ready", true}}).find("Search: ready") != std::string::npos);
        const auto warned = local_status_text({{"command", "status"}, {"local_ready", true},
            {"project_quality", "degraded"}, {"quality_warning", {{"state", "degraded"}}}});
        require(warned.find("Search: ready") != std::string::npos && warned.find("Search warning:") != std::string::npos);
        const auto unsafe = local_status_text({{"command", "status"}, {"root", "/root\n\033[2J"}});
        require(unsafe.find('\033') == std::string::npos && unsafe.find("\\n\\u001b[2J") != std::string::npos);
        const auto error = local_status_text({{"error", {{"code", "service-unavailable"}, {"message", "not running\n\033"}}}});
        require(error.find("Shin status unavailable:") != std::string::npos && error.find('\033') == std::string::npos);
        std::cout << "Headless status: ready/standby/blocked/preparing, errors, missing metrics and terminal escaping passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
