#include "shin/observer/local_worker.h"
#include "shin/observer/local_service.h"
#include "shin/observer/local_rpc.h"
#include "shin/windows/transport.h"
#include "../../tests/core/windows_private_acl.h"
#include <windows.h>
#include <fstream>
#include <iostream>

using namespace shin::observer;
namespace native = shin::windows;
using Clock = std::chrono::steady_clock;
auto deadline(int seconds = 5) { return Clock::now() + std::chrono::seconds(seconds); }
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void write(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary); out << content; out.close(); check(!out.fail(), "fixture write failed");
}
template<class F> void refused(F action, const std::string& code) {
    try { action(); }
    catch (const Error& error) {
        if (error.code != code) throw std::runtime_error("expected " + code + ", got " + error.code);
        return;
    }
    throw std::runtime_error("expected refusal " + code);
}
struct Fixture {
    fs::path path = fs::canonical(fs::temp_directory_path()) / ("shin-worker-win-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(Clock::now().time_since_epoch().count()));
    Fixture() { ensure_private_directory(path); }
    ~Fixture() { std::error_code ignored; fs::remove_all(path, ignored); }
};
Json payload(const LocalProjectRequest& request) {
    auto value = local_rpc::reply("fixture");
    value.update({{"command", request.command}, {"root", request.root.string()}, {"home", request.home.string()},
        {"root_identity", directory_identity(request.root)},
        {"workspace_identity", directory_identity(request.home / "projects" / digest(request.root.string()), true)},
        {"query", request.query}, {"path", request.path}, {"record", request.record}, {"snapshot", request.snapshot}});
    return value;
}
Json worker_exchange(const fs::path& executable, const Json& request) {
    native::Worker process(executable, "_service-worker");
    process.input().write_frame(request.dump(), 16384, deadline());
    const auto result = parse_json(process.output().read_frame(128 * 1024, deadline()));
    check(!process.input().wait_input(Clock::now() + std::chrono::milliseconds(20)),
        "retired cold metric channel is still active");
    process.output().close();
    check(process.wait(deadline()) == 0, "worker did not exit after reply consumption");
    return result;
}
Json preparation(const LocalProjectRequest& request) {
    const auto bound = payload(request);
    return {{"schema_version", "shin.local-resident.v1"}, {"version", SHIN_OBSERVER_VERSION}, {"core_version", SHIN_VERSION},
        {"root", bound.at("root")}, {"home", bound.at("home")}, {"root_identity", bound.at("root_identity")},
        {"workspace_identity", bound.at("workspace_identity")}};
}
Json prepare(native::Worker& process, const LocalProjectRequest& request) {
    process.input().write_frame(preparation(request).dump(), 8192, deadline());
    const auto ready = parse_json(process.output().read_frame(128 * 1024, deadline()));
    check(ready.at("kind") == "ready", "resident preparation failed");
    return ready.at("preparation");
}
Json resident_exchange(native::Worker& process, const LocalProjectRequest& request) {
    auto input = payload(request);
    for (const auto* field : {"home", "root_identity", "workspace_identity"}) input.erase(field);
    process.input().write_frame(input.dump(), 8192, deadline());
    const auto result = parse_json(process.output().read_frame(128 * 1024, deadline()));
    check(result.at("kind") == "response", "resident reply kind");
    const auto& metric = result.at("metric");
    check(metric.size() == (metric.contains("phase_ms") ? 6 : 5) &&
        metric.at("response_bytes") == result.at("response").dump().size() + 1 &&
        metric.at("ok") == result.at("response").at("ok"), "invalid/private resident metric");
    return result;
}
void close_resident(native::Worker& process) {
    process.input().close(); process.output().close();
    check(process.wait(deadline()) == 0, "resident did not exit after parent closed");
}
void residents(const fs::path& executable, const LocalProjectRequest& first, const LocalProjectRequest& second) {
    for (const auto* field : {"version", "workspace_identity", "root_identity"}) {
        native::Worker invalid(executable, "_resident-worker");
        auto input = preparation(first); input[field] = "";
        invalid.input().write_frame(input.dump(), 8192, deadline());
        const auto result = parse_json(invalid.output().read_frame(128 * 1024, deadline()));
        check(result.at("kind") == "preparation-failed", "resident accepted missing identity");
        invalid.output().close(); check(invalid.wait(deadline()) != 0, "invalid preparation survived");
    }
    {
        native::Worker process(executable, "_resident-worker"); prepare(process, first);
        const auto wrong = resident_exchange(process, second);
        check(wrong.at("reusable") == false && wrong.at("response").at("error").at("code") == "service-project-denied",
            "resident crossed project binding");
        close_resident(process);
    }
    {
        native::Worker process(executable, "_resident-worker"); prepare(process, first);
        process.input().write_frame("{", 8192, deadline());
        const auto result = parse_json(process.output().read_frame(128 * 1024, deadline()));
        check(result.at("reusable") == false && result.at("response").at("ok") == false, "malformed resident frame accepted");
        close_resident(process);
    }
    native::Worker a(executable, "_resident-worker"), b(executable, "_resident-worker");
    const auto initial = prepare(a, first); const auto other = prepare(b, second);
    check(initial.at("snapshot_id") != other.at("snapshot_id"), "resident snapshots mixed");
    const auto pid = a.process_id();
    for (int i = 1; i <= 3; ++i) {
        const auto answer = resident_exchange(a, first);
        const auto& result = answer.at("response").at("result");
        check(answer.at("reusable") == true && result.at("snapshot_id") == initial.at("snapshot_id") &&
            result.at("source_context").get<std::string>().find("return 17") != std::string::npos, "resident changed generation/source");
        check(result.at("work_performed").at("full_generations") == 0 && result.at("work_performed").at("resident_hit") == true &&
            result.at("work_performed").at("request_sequence") == i && a.process_id() == pid, "resident rebuilt or lost prepared state");
    }
    auto read = first; read.command = "read"; read.path = "quota.cpp"; read.snapshot = "fnv1a64:0000000000000000";
    auto result = resident_exchange(a, read);
    check(result.at("reusable") == true && result.at("response").at("error").at("code") == "source-snapshot-mismatch",
        "snapshot error retired reusable resident");
    read.snapshot = initial.at("snapshot_id");
    check(resident_exchange(a, read).at("response").at("result").at("native_read_complete") == true, "resident snapshot-bound read");
    auto denied = first; denied.command = "init";
    result = resident_exchange(a, denied);
    check(result.at("reusable") == true && result.at("response").at("error").at("code") == "service-method-denied",
        "resident allowed init");
    write(first.root / "quota.cpp", "int quotaLimit() { return 8181; }\n");
    const auto invalidated = parse_json(a.output().read_frame(128 * 1024, deadline()));
    check(invalidated.at("kind") == "invalidated" && invalidated.at("code") == "project-updating", "idle edit did not invalidate resident");
    close_resident(a);
    const auto unchanged = resident_exchange(b, second).at("response").at("result");
    check(unchanged.at("snapshot_id") == other.at("snapshot_id") &&
        unchanged.at("source_context").get<std::string>().find("return 99") != std::string::npos, "edit invalidated unrelated project");
    close_resident(b);
}
void scenario() {
    Fixture fixture;
    const auto executable = fixture.path / L"worker \u043a\u043e\u0434.exe";
    fs::copy_file(current_executable_path(), executable); fixture_private_acl(executable);
    LocalProjectRequest first;
    first.home = fixture.path / "home"; ensure_private_directory(first.home);
    first.root = fixture.path / "first"; first.profile_dir = fs::path(SHIN_TEST_PROFILES); first.profile_dir_explicit = true;
    auto second = first; second.root = fixture.path / L"\u0432\u0442\u043e\u0440\u043e\u0439";
    for (auto* request : {&first, &second}) {
        ensure_private_directory(request->root);
        write(request->root / "README.md", "# Quota\n\nquotaLimit owns the local quota.\n");
        write(request->root / "quota.cpp", request == &first ? "int quotaLimit() { return 17; }\n" : "int quotaLimit() { return 99; }\n");
        request->command = "init"; check(local_project(*request).at("local_ready") == true, "worker fixture init");
        request->command = "query"; request->query = "quotaLimit";
    }
    for (auto* request : {&first, &second}) {
        const auto expected = request == &first ? "return 17" : "return 99";
        const auto answer = request_local_worker(*request, executable);
        check(answer.at("source_context").get<std::string>().find(expected) != std::string::npos, "worker mixed project source");
        auto read = *request; read.command = "read"; read.path = "quota.cpp"; read.snapshot = answer.at("snapshot_id");
        check(request_local_worker(read, executable).at("native_read_complete") == true, "worker bound read failed");
        read.snapshot = "fnv1a64:0000000000000000";
        refused([&] { request_local_worker(read, executable); }, "source-snapshot-mismatch");
        check(qualify_local_project(*request, executable).at("local_ready") == true, "native qualification worker failed");
    }
    auto wrong = first; wrong.maintenance_root_identity = directory_identity(second.root);
    refused([&] { request_local_worker(wrong, executable); }, "refresh-root-changed");
    wrong = first; wrong.command = "init";
    refused([&] { request_local_worker(wrong, executable); }, "service-method-denied");
    refused([&] { request_local_worker(first, executable, [] { return true; }); }, "service-request-cancelled");
    auto raw = payload(first);
    check(worker_exchange(executable, raw).at("ok") == true, "read worker RPC failed");
    raw["root_identity"] = "";
    check(worker_exchange(executable, raw).at("error").at("code") == "service-request-invalid", "worker accepted empty authority");
    raw = payload(first); raw["command"] = "service-stop";
    check(worker_exchange(executable, raw).at("error").at("code") == "service-method-denied", "worker accepted service control");
    raw = payload(first); raw["observer_version"] = "old";
    check(worker_exchange(executable, raw).at("error").at("code") == "service-version-mismatch", "worker accepted old protocol");
    raw = payload(first); raw["unexpected"] = true;
    check(worker_exchange(executable, raw).at("error").at("code") == "service-request-invalid", "worker accepted extra fields");
    residents(executable, first, second);
    const auto stale = request_local_worker(first, executable);
    check(stale.at("ok") == false && !stale.contains("source_context"), "worker returned stale source");
    const auto watched = watch_local_projects(first, {first.root, second.root}, 2, [] { return false; }, {}, {}, {},
        [&](const LocalProjectRequest& request) { return request_local_worker(request, executable, request.maintenance_stop); });
    for (const auto& state : watched.at("projects")) {
        check(state.at("failures") == 0 && state.at("last_result").at("phase") == "current", "owned refresh process failed");
    }
    check(request_local_worker(first, executable).at("source_context").get<std::string>().find("return 8181") != std::string::npos,
        "worker did not read refreshed generation");
    check(request_local_worker(second, executable).at("source_context").get<std::string>().find("return 99") != std::string::npos,
        "refresh changed unrelated project");
    {
        native::Worker current(executable, "_resident-worker"); prepare(current, first);
        check(resident_exchange(current, first).at("response").at("result").at("source_context").get<std::string>().find("return 8181") != std::string::npos,
            "reprepared resident returned obsolete source");
        close_resident(current);
    }
    {
        native::Worker idle(executable, "_service-worker");
        check(idle.wait(deadline()) != 0, "missing worker input was not bounded");
    }
    {
        native::Worker blocked(executable, "_service-worker");
        try { (void)blocked.wait(deadline(), [] { return true; }); }
        catch (const native::TransportError& error) { check(error.code == "service-request-cancelled", "wrong cancellation code"); }
        check(blocked.wait(deadline()) != STILL_ACTIVE, "cancelled worker survived");
    }
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "_service-worker") return local_service_worker();
        if (argc == 2 && std::string_view(argv[1]) == "_refresh-worker") return local_refresh_worker();
        if (argc == 2 && std::string_view(argv[1]) == "_resident-worker") return local_resident_worker();
        if (argc != 1) return 2;
        scenario();
        std::cout << "native owned project read/refresh/resident workers passed\n"; return 0;
    } catch (const Error& error) { std::cerr << error.code << ": " << error.what() << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
