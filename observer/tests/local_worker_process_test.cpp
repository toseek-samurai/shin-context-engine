#include "shin/observer/local_worker_process.h"
#include "shin/observer/local_worker.h"
#include "shin/observer/local_service.h"
#include "shin/observer/local_rpc.h"
#include <fstream>
#include <iostream>
#include <cmath>
#include <limits>
#include <vector>
#ifdef __linux__
#include <time.h>
#endif
#ifdef _WIN32
#include "../../tests/core/windows_private_acl.h"
#else
#include <unistd.h>
#endif

namespace {
using namespace shin::observer;

void expect(bool value) {
    if (!value) throw std::runtime_error("local suspend observation");
}

LocalSuspendObservation scripted(std::vector<std::optional<SleepSample>> samples) {
    auto index = std::make_shared<std::size_t>(0);
    return LocalSuspendObservation([samples = std::move(samples), index]() mutable {
        if (*index >= samples.size()) return std::optional<SleepSample>{};
        return samples[(*index)++];
    });
}

void suspend_observation() {
    const auto native = native_suspend_sample();
#if defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
    expect(native.has_value());
    expect(std::isfinite(native->lower_ms) && std::isfinite(native->upper_ms) &&
        native->lower_ms >= 0.0 && native->upper_ms >= native->lower_ms);
#else
    expect(!native.has_value());
#endif

    // Ordinary elapsed time under load advances both clocks together.
    auto normal = scripted({SleepSample{1000, 1002}, SleepSample{1000, 1002}});
    expect(normal.available() && normal.suspended_ms() == 0.0);

    // A discontinuity in the sleep-including clock is observable as a lower bound.
    auto slept = scripted({SleepSample{1000, 1001}, SleepSample{1350, 1352}});
    expect(slept.suspended_ms() >= 349.0);

    // Overlapping wide bounds cannot establish that suspend time increased.
    auto jitter = scripted({SleepSample{100, 200}, SleepSample{150, 250}});
    expect(jitter.suspended_ms() == 0.0);

    // Unsupported baseline leaves the observation inert.
    auto unsupported = scripted({std::nullopt, SleepSample{1000, 1001}});
    expect(!unsupported.available() && unsupported.suspended_ms() == 0.0);

    LocalSuspendObservation unavailable([]() -> std::optional<SleepSample> {
        throw std::runtime_error("unavailable clock");
    });
    expect(!unavailable.available() && unavailable.suspended_ms() == 0.0);

    // Regressive and malformed samples are ignored; a later valid sample works.
    auto regressive = scripted({SleepSample{200, 210}, SleepSample{100, 110},
        SleepSample{std::numeric_limits<double>::quiet_NaN(), 300}, SleepSample{350, 360}});
    expect(regressive.suspended_ms() == 0.0);
    expect(regressive.suspended_ms() == 0.0);
    expect(regressive.suspended_ms() >= 140.0);

    // Missing or invalid follow-up evidence preserves any prior lower bound.
    auto retained = scripted({SleepSample{10, 11}, SleepSample{150, 151}, std::nullopt,
        SleepSample{std::numeric_limits<double>::infinity(), 500}});
    expect(retained.suspended_ms() >= 139.0);
    const double prior = retained.suspended_ms();
    expect(retained.suspended_ms() == prior);
}
}

using namespace shin::observer;
using Clock = LocalWorkerProcess::Clock;
auto deadline(int seconds = 10) { return Clock::now() + std::chrono::seconds(seconds); }
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void write(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary); out << content; out.close(); check(!out.fail(), "fixture write");
}
struct Fixture {
    fs::path path = fs::canonical(fs::temp_directory_path()) /
        ("shin-worker-process-" + std::to_string(Clock::now().time_since_epoch().count()));
    Fixture() { ensure_private_directory(path, true); }
    ~Fixture() { std::error_code ignored; fs::remove_all(path, ignored); }
};
Json preparation(const LocalProjectRequest& request) {
    return {{"schema_version", "shin.local-resident.v1"}, {"version", SHIN_OBSERVER_VERSION}, {"core_version", SHIN_VERSION},
        {"root", request.root.string()}, {"home", request.home.string()}, {"root_identity", directory_identity(request.root)},
        {"workspace_identity", directory_identity(request.home / "projects" / digest(request.root.string()), true)}};
}
Json query(const LocalProjectRequest& request) {
    auto result = local_rpc::reply("fixture");
    result.update({{"root", request.root.string()}, {"command", "query"}, {"query", "quotaLimit"},
        {"path", ""}, {"record", ""}, {"snapshot", ""}});
    return result;
}
Json refresh(const LocalProjectRequest& request, const fs::path& executable,
             const std::function<void(const Json&)>& progress = {}) {
    LocalWorkerProcess child(executable, LocalWorkerRole::refresh);
    Json payload{{"schema_version", "shin.local-refresh.v1"}, {"version", SHIN_OBSERVER_VERSION}, {"core_version", SHIN_VERSION},
        {"root", request.root.string()}, {"home", request.home.string()}, {"root_identity", request.maintenance_root_identity},
        {"workspace_identity", request.maintenance_workspace_identity}, {"worktree", request.maintenance_worktree}};
    if (progress) payload["diagnostics"] = true;
    child.send(payload, deadline());
    const auto result = child.receive(deadline(), request.maintenance_stop, progress);
    child.stop();
    return result;
}
void scenario() {
    Fixture fixture;
    const auto executable = fixture.path / "worker.exe";
    fs::copy_file(current_executable_path(), executable);
#ifdef _WIN32
    fixture_private_acl(executable);
#endif
    {
        const auto stalled = fixture.path / "progress-fixture.exe";
        fs::copy_file(current_executable_path(), stalled);
#ifdef _WIN32
        fixture_private_acl(stalled);
#endif
        LocalWorkerProcess timeout_worker(stalled, LocalWorkerRole::refresh);
        timeout_worker.send({{"diagnostics", true}}, deadline());
        std::string last_stage;
        // Include cold copied-executable startup on macOS in this fixture's
        // budget; the worker still never completes and must hit one deadline.
        bool timed_out = false;
        try {
            (void)timeout_worker.receive(Clock::now() + std::chrono::seconds(2), {}, [&](const Json& frame) {
                last_stage = frame.value("stage", "");
            });
        } catch (const Error& error) { timed_out = error.code == "service-timeout"; }
        timeout_worker.stop();
        check(timed_out && last_stage == "candidate-validation", "refresh deadline lost last-stage progress evidence");

        double sleep_offset = 0;
        const auto sample = [&]() -> std::optional<SleepSample> {
            return SleepSample{sleep_offset, sleep_offset};
        };
        LocalWorkerProcess suspended_worker(stalled, LocalWorkerRole::refresh, sample);
        suspended_worker.send({{"diagnostics", true}}, deadline());
        sleep_offset = 255000;
        bool suspended = false;
        bool pumped_after_sleep = false;
        try {
            (void)suspended_worker.receive(Clock::now() - std::chrono::seconds(1), [&] {
                pumped_after_sleep = true; return false;
            });
        } catch (const Error& error) { suspended = error.code == "refresh-suspended"; }
        suspended_worker.stop();
        check(suspended && !pumped_after_sleep, "sleep must interrupt refresh before expired deadline or client pump");
        LocalWorkerProcess read_worker(stalled, LocalWorkerRole::read, sample);
        sleep_offset += 255000;
        bool read_timeout = false;
        try { (void)read_worker.receive(Clock::now() - std::chrono::seconds(1)); }
        catch (const Error& error) { read_timeout = error.code == "service-timeout"; }
        check(read_timeout, "sleep detection must not reclassify read deadlines");

        LocalWorkerProcess cancelled_worker(stalled, LocalWorkerRole::refresh);
        cancelled_worker.send({{"diagnostics", true}}, deadline());
        bool saw_progress = false;
        bool cancelled = false;
        try {
            (void)cancelled_worker.receive(deadline(), [&] { return saw_progress; }, [&](const Json&) { saw_progress = true; });
        } catch (const Error& error) { cancelled = error.code == "service-request-cancelled"; }
        cancelled_worker.stop();
        check(cancelled && saw_progress, "refresh progress cancellation was not observed");
    }
    {
        const auto wire = fixture.path / "wire-fixture.exe";
        fs::copy_file(current_executable_path(), wire);
#ifdef _WIN32
        fixture_private_acl(wire);
#endif
        for (const auto role : {LocalWorkerRole::refresh, LocalWorkerRole::read}) {
            LocalWorkerProcess malformed(wire, role);
            bool rejected = false;
            try { (void)malformed.receive(deadline()); }
            catch (const Error& error) { rejected = error.code == "service-frame-invalid"; }
            check(rejected, "one-shot worker accepted concatenated replies");
        }
    }
    LocalProjectRequest first; first.home = fixture.path / "home"; ensure_private_directory(first.home);
    first.root = fixture.path / "first"; first.profile_dir = fs::path(SHIN_TEST_PROFILES); first.profile_dir_explicit = true;
    auto second = first; second.root = fixture.path / "second";
    for (auto* project : {&first, &second}) {
        ensure_private_directory(project->root);
        write(project->root / "README.md", "# Quota\n\nquotaLimit owns the quota.\n");
        write(project->root / "quota.cpp", project == &first ? "int quotaLimit() { return 17; }\n" : "int quotaLimit() { return 99; }\n");
        project->command = "init"; check(local_project(*project).at("local_ready") == true, "init failed");
    }
    {
        auto read = first; read.command = "query"; read.query = "quotaLimit";
        check(request_local_worker(read, executable).at("source_context").get<std::string>().find("return 17") != std::string::npos,
            "common cold worker failed");
        check(qualify_local_project(read, executable).at("local_ready") == true, "common qualification failed");
        bool cancelled = false;
        try { (void)request_local_worker(read, executable, [] { return true; }); }
        catch (const Error& error) { cancelled = error.code == "service-request-cancelled"; }
        check(cancelled, "cold worker ignored cancellation");
        read.command = "init";
        bool denied = false;
        try { (void)request_local_worker(read, executable); }
        catch (const Error& error) { denied = error.code == "service-method-denied"; }
        check(denied, "cold worker admitted enrollment");
    }
    {
        LocalWorkerProcess waiting(executable, LocalWorkerRole::resident);
        LocalWorkerProcess ready(executable, LocalWorkerRole::resident);
        std::uint64_t pumps = 0;
        const auto pump = [&] { check(!waiting.poll().frame, "unprepared worker replied"); ++pumps; return false; };
        ready.send(preparation(second), deadline(), pump);
        check(ready.receive(deadline(), pump).at("kind") == "ready", "independent preparation failed");
        ready.send(query(second), deadline(), pump);
        const auto result = ready.receive(deadline(), pump);
        check(pumps >= 2 && result.at("response").at("result").at("source_context").get<std::string>().find("return 99") != std::string::npos,
            "waiting worker blocked or mixed another project");
        const auto rss = ready.rss_bytes();
        check(rss && *rss > 0 && ready.process_id() != waiting.process_id(), "owned worker RAM/identity unavailable");
        bool cancelled = false;
        try { (void)waiting.receive(deadline(), [] { return true; }); }
        catch (const Error& error) { cancelled = error.code == "service-request-cancelled"; }
        check(cancelled, "receive cancellation ignored");
        waiting.stop(); waiting.stop();
        check(!waiting.rss_bytes(), "stopped worker still reports live memory");
    }
    {
        LocalWorkerProcess resident(executable, LocalWorkerRole::resident);
        resident.send(preparation(first), deadline());
        const auto initial = resident.receive(deadline()); check(initial.at("kind") == "ready", "preparation failed");
        for (int i = 1; i <= 3; ++i) {
            resident.send(query(first), deadline());
            const auto response = resident.receive(deadline());
            const auto& work = response.at("response").at("result").at("work_performed");
            check(work.at("full_generations") == 0 && work.at("resident_hit") == true && work.at("request_sequence") == i,
                "shared process lost resident session");
        }
        write(first.root / "quota.cpp", "int quotaLimit() { return 8181; }\n");
        check(resident.receive(deadline()).at("kind") == "invalidated", "idle source invalidation missing");
    }
    const auto watched = watch_local_projects(first, {first.root, second.root}, 2, [] { return false; }, {}, {}, {},
        [&](const LocalProjectRequest& request) { return refresh(request, executable); });
    for (const auto& project : watched.at("projects"))
        check(project.at("failures") == 0 && project.at("last_result").at("phase") == "current", "shared refresh failed");
    {
        std::uint64_t refresh_calls = 0;
        const auto suspended_retry = watch_local_projects(first, {first.root}, 3, [] { return false; }, {}, {}, {},
            [&](const LocalProjectRequest& request) -> Json {
                ++refresh_calls;
                if (refresh_calls == 2) throw Error("refresh-suspended", "fixture suspension");
                if (refresh_calls == 3) {
                    std::ifstream input(first.home / "projects" / digest(first.root.string()) / "watch/state.json");
                    Json watch_state; input >> watch_state;
                    check(bool(input), "read suspended watch state");
                    check(watch_state.at("last_result").at("phase") == "coalescing" &&
                          watch_state.at("last_result").at("retry_reason") == "refresh-suspended" &&
                          watch_state.at("suspended_refreshes") == 1 && watch_state.at("failures") == 0,
                          "suspended refresh did not coalesce without counting as failure");
                }
                return refresh(request, executable);
            });
        const auto& retry_state = suspended_retry.at("watch");
        check(refresh_calls == 4 && retry_state.at("last_result").at("phase") == "current" &&
              retry_state.at("suspended_refreshes") == 1 && retry_state.at("failures") == 0,
              "suspended refresh did not retry to readiness");
    }
    {
        LocalProjectRequest request = first;
        request.maintenance_root_identity = directory_identity(request.root);
        request.maintenance_workspace_identity = directory_identity(request.home / "projects" / digest(request.root.string()), true);
        std::size_t progress_frames = 0;
        const auto result = refresh(request, executable, [&](const Json& frame) {
            check(frame.value("kind", "") == "progress" && frame.at("stage").is_string(), "invalid refresh progress frame");
            ++progress_frames;
        });
        check(result.at("ok") == true && progress_frames > 0, "opt-in refresh progress missing");
        bool saw_progress = false;
        try {
            LocalWorkerProcess child(executable, LocalWorkerRole::refresh);
            Json payload{{"schema_version", "shin.local-refresh.v1"}, {"version", SHIN_OBSERVER_VERSION},
                {"core_version", SHIN_VERSION}, {"root", request.root.string()}, {"home", request.home.string()},
                {"root_identity", request.maintenance_root_identity}, {"workspace_identity", request.maintenance_workspace_identity},
                {"worktree", request.maintenance_worktree}, {"diagnostics", true}};
            child.send(payload, deadline());
            (void)child.receive(deadline(), [&] { return saw_progress; }, [&](const Json&) { saw_progress = true; });
            check(false, "progress cancellation was ignored");
        } catch (const Error& error) {
            check(error.code == "service-request-cancelled", "progress cancellation returned wrong error");
        }
        check(saw_progress, "cancellation test did not receive a progress frame");
    }
    for (const auto* project : {&first, &second}) {
        LocalWorkerProcess resident(executable, LocalWorkerRole::resident);
        resident.send(preparation(*project), deadline()); check(resident.receive(deadline()).at("kind") == "ready", "reprepare failed");
        resident.send(query(*project), deadline());
        const auto result = resident.receive(deadline()).at("response").at("result");
        check(result.at("source_context").get<std::string>().find(project == &first ? "return 8181" : "return 99") != std::string::npos,
            "shared process mixed refreshed sources");
    }
}
int progress_timeout_worker() {
    std::cout << Json{{"kind", "progress"}, {"stage", "candidate-validation"}, {"elapsed_ms", 140.0}}.dump() << '\n' << std::flush;
#ifdef _WIN32
    ::Sleep(INFINITE);
#else
    for (;;) ::pause();
#endif
    return 0;
}
int invalid_wire_worker() {
    // One native write makes the malformed burst deterministic. The parent
    // owns termination, so pipe buffers survive until its parser rejects them.
    constexpr char bytes[] = "{}\n{}\n";
#ifdef _WIN32
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    const auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(event != nullptr, "wire fixture event");
    OVERLAPPED operation{}; operation.hEvent = event;
    DWORD written = 0;
    bool ok = WriteFile(output, bytes, sizeof(bytes) - 1, &written, &operation) != 0;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) CancelIoEx(output, &operation);
        ok = GetOverlappedResult(output, &operation, &written, TRUE) != 0;
    }
    CloseHandle(event);
    check(ok && written == sizeof(bytes) - 1, "wire fixture write");
    WaitForSingleObject(GetCurrentProcess(), INFINITE);
#else
    check(::write(STDOUT_FILENO, bytes, sizeof(bytes) - 1) == static_cast<ssize_t>(sizeof(bytes) - 1), "wire fixture write");
    for (;;) ::pause();
#endif
    return 1;
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && fs::path(argv[0]).filename() == "progress-fixture.exe") return progress_timeout_worker();
        if (argc == 2 && fs::path(argv[0]).filename() == "wire-fixture.exe") return invalid_wire_worker();
        if (argc == 2 && std::string_view(argv[1]) == "_resident-worker") return local_resident_worker();
        if (argc == 2 && std::string_view(argv[1]) == "_refresh-worker") return local_refresh_worker();
        if (argc == 2 && std::string_view(argv[1]) == "_service-worker") return local_service_worker();
        if (argc != 1) return 2;
        suspend_observation();
        scenario(); std::cout << "shared worker pump, isolation, refresh and RAM passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
