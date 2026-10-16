#include "shin/observer/local_project.h"
#include "shin/observer/local_query.h"
#include "shin/observer/local_identity.h"
#include "context_internal.h"
#include <windows.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>

using namespace shin::observer;
using Clock = std::chrono::steady_clock;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void write(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary); out << content; out.close();
    check(!out.fail(), "fixture write failed");
}
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("shin-project-win-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(Clock::now().time_since_epoch().count()));
    Fixture() { ensure_private_directory(root); }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};
template<class Action> void refused(Action action, const std::string& code) {
    try { action(); }
    catch (const Error& error) {
        if (error.code != code) throw std::runtime_error("expected " + code + ", received " + error.code);
        return;
    }
    throw std::runtime_error("unsafe project operation succeeded");
}
std::shared_ptr<LocalQuerySession> resident(LocalProjectRequest request) {
    std::shared_ptr<LocalQuerySession> session;
    request.command = "status"; request.query_session = &session;
    const auto status = local_project(request);
    if (status.at("local_ready") != true || !session)
        throw std::runtime_error("resident preparation failed: " + status.dump());
    return session;
}
void read_value(LocalProjectRequest request, LocalQuerySession& session, const std::string& expected) {
    request.command = "query"; request.query = "quotaLimit request quota";
    const auto answer = session.execute(request);
    check(answer.at("source_context").get<std::string>().find(expected) != std::string::npos, "wrong project value");
    check(answer.at("work_performed").at("full_generations") == 0, "resident query rebuilt the corpus");
    request.command = "read"; request.path = "quota.cpp"; request.snapshot = answer.at("snapshot_id");
    const auto read = session.execute(request);
    check(read.at("native_read_complete") == true &&
        read.at("source_context").get<std::string>().find(expected) != std::string::npos, "bound source read failed");
    request.snapshot = "fnv1a64:0000000000000000";
    refused([&] { session.execute(request); }, "source-snapshot-mismatch");
}
int main() {
    try {
        Fixture fixture;
        LocalProjectRequest request;
        request.root = fixture.root / "project"; request.home = fixture.root / "home";
        ensure_private_directory(request.root); ensure_private_directory(request.home);
        write(request.root / "README.md", "# Quota service\n\nquotaLimit is the local request quota.\n");
        write(request.root / "quota.cpp", "int quotaLimit() { return 17; }\n");
        request.profile_dir = fs::path(SHIN_TEST_PROFILES); request.profile_dir_explicit = true;
        request.command = "init";
        auto result = local_project(request);
        check(result.at("ok") == true, "native init failed");
        request.command = "status";
        result = local_project(request);
        check(result.at("ok") == true, "native status failed");
        request.command = "query"; request.query = "quotaLimit request quota";
        result = local_project(request);
        check(result.at("ok") == true && result.dump().find("quotaLimit") != std::string::npos, "native query failed");
        const auto original_snapshot = result.at("snapshot_id");
        auto second = request;
        second.root = fixture.root / L"project-\u0434\u0432\u0430";
        ensure_private_directory(second.root);
        write(second.root / "README.md", "# Quota service\n\nquotaLimit owns a different quota.\n");
        write(second.root / "quota.cpp", "int quotaLimit() { return 9909; }\n");
        second.command = "init";
        check(local_project(second).at("local_ready") == true, "second project init failed");
        const auto first_identity = local_identity(request, false);
        const auto second_identity = local_identity(second, false);
        check(first_identity.at("installation_id") == second_identity.at("installation_id") &&
            first_identity.at("local_workspace_id") != second_identity.at("local_workspace_id"),
            "project identities are not isolated within one installation");
        auto one = resident(request), two = resident(second);
        read_value(request, *one, "return 17"); read_value(second, *two, "return 9909");
        auto foreign = request; foreign.root = second.root; foreign.command = "query";
        refused([&] { one->execute(foreign); }, "service-project-denied");
        write(request.root / "quota.cpp", "int quotaLimit() { return 8181; }\n");
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (one->observed_unchanged()) {
            check(Clock::now() < deadline, "edited source did not invalidate resident");
            std::this_thread::yield();
        }
        request.command = "query";
        refused([&] { one->execute(request); }, "project-updating");
        read_value(second, *two, "return 9909");
        one.reset(); two.reset();
        const auto stale = local_project(request);
        check(stale.at("ok") == false && !stale.contains("source_context"), "stale project leaked old evidence");
        std::map<fs::path, bool> refreshed;
        // First observation coalesces the edit; the matching second observation publishes it.
        const auto watched = watch_local_projects(request, {request.root, second.root}, 2, [] { return false; }, {}, {},
            [&](const fs::path& path, const Json& state) {
                if (state.at("ticks") != 0) refreshed[path] = state.at("failures") == 0 && state.at("phase") == "current";
            });
        check(watched.at("ok") == true && refreshed.size() == 2 && refreshed[request.root] && refreshed[second.root],
            "shared two-root watch failed");
        one = resident(request); two = resident(second);
        check(one->description().at("snapshot_id") != original_snapshot, "watch did not publish new generation");
        read_value(request, *one, "return 8181"); read_value(second, *two, "return 9909");
        check(local_identity(request, false) == first_identity && local_identity(second, false) == second_identity,
            "refresh changed provenance IDs");
        one.reset(); two.reset();
        const auto candidate = fixture.root / "candidate";
        ensure_private_directory(candidate, true);
        refused([&] { ensure_private_directory(candidate, true); }, "unsafe-directory");
        const auto config = load_config(request.home / "projects" / digest(request.root.string()) / "config/state.json");
        {
            StateStore store(config.state_dir, true);
            auto saved = store.load().value();
            auto bad = saved; bad["root_identity"] = "win-volume-v1:invalid";
            store.save(bad);
            refused([&] { observer_status(config); }, "invalid-state");
            store.save(saved);
        }
        check(observer_status(config).contains("state"), "valid Windows observation did not reload");
        std::cout << "native Observer two-root init/watch/resident/query/read passed\n";
        return 0;
    } catch (const Error& error) {
        std::cerr << error.code << ": " << error.what() << '\n'; return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
