#include "shin/observer/local_project.h"
#include "shin/observer/local_worker.h"
#include "shin/observer/local_worker_process.h"
#include "shin/observer/local_service.h"
#include "context_internal.h"
#include "../src/local_refresh_guard.h"
#include "../src/local_refresh_session.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#ifdef _WIN32
#include "../../tests/core/windows_private_acl.h"
#endif

namespace {
using namespace shin::observer;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void write(const fs::path& path, const std::string& value) {
    std::ofstream out(path, std::ios::binary); out << value; out.close();
    check(!out.fail(), "fixture write failed");
}
struct Fixture {
    fs::path base = fs::canonical(fs::temp_directory_path());
    fs::path path = base / ("shin-idle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { ensure_private_directory(path, true); }
    ~Fixture() {
        if (path.parent_path() == base && path.filename().string().starts_with("shin-idle-")) {
            std::error_code ignored; fs::remove_all(path, ignored);
        }
    }
};
bool hit(const Json& result) { return result.value("refresh_work", Json::object()).value("idle_checkpoint_hit", false); }
Json payload(const LocalProjectRequest& request, const std::string& worktree = "") {
    return {{"schema_version", "shin.local-refresh.v1"}, {"version", SHIN_OBSERVER_VERSION}, {"core_version", SHIN_VERSION},
        {"root", request.root.string()}, {"home", request.home.string()}, {"root_identity", directory_identity(request.root)},
        {"workspace_identity", directory_identity(request.home / "projects" / digest(request.root.string()), true)}, {"worktree", worktree}};
}
Json current(LocalRefreshWorker& worker, const LocalProjectRequest& request) {
    std::string worktree;
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto result = worker.execute(payload(request, worktree), [] { return false; });
        if (!result.value("ok", false)) throw std::runtime_error(result.dump());
        if (result.value("phase", "") == "current") return result;
        check(result.value("phase", "") == "coalescing", "unexpected refresh state");
        worktree = result.at("observation").at("worktree_id");
    }
    throw std::runtime_error("refresh did not settle");
}
Json warm(LocalRefreshWorker& worker, const LocalProjectRequest& request) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto result = current(worker, request);
        if (hit(result)) return result;
    }
    throw std::runtime_error("unchanged refresh never reused its checkpoint");
}
LocalProjectRequest project(const fs::path& base, const fs::path& profiles, const std::string& name, int value) {
    LocalProjectRequest request; request.root = base / name; request.home = base / (name + "-home");
    ensure_private_directory(request.root, true); ensure_private_directory(request.home, true);
    write(request.root / "README.md", "# Quota\nquotaLimit owns the request quota.\n");
    write(request.root / "quota.cpp", "int quotaLimit() { return " + std::to_string(value) + "; }\n");
    request.profile_dir = profiles; request.profile_dir_explicit = true; request.command = "init";
    check(local_project(request).at("local_ready") == true, "initial preparation failed");
    return request;
}
void descriptor_pressure(const fs::path& base, const fs::path& profiles) {
#ifdef __APPLE__
    // WatchRoot retains descriptors for ancestors, not just the selected roots.
    // Exercise one long-lived worker under launchd's ordinary 256-descriptor
    // soft limit. Tiny, deep projects reproduce pressure without a large corpus.
    auto deep = base;
    for (unsigned i = 0; i < 12; ++i) {
        deep /= "nested";
        ensure_private_directory(deep, true);
    }
    std::vector<LocalProjectRequest> projects;
    for (unsigned i = 0; i < 9; ++i)
        projects.push_back(project(deep, profiles, "scope-" + std::to_string(i), 100 + i));
    struct Limit {
        rlimit saved{};
        Limit() {
            check(::getrlimit(RLIMIT_NOFILE, &saved) == 0, "cannot read descriptor limit");
            auto bounded = saved; bounded.rlim_cur = std::min<rlim_t>(saved.rlim_cur, 256);
            check(::setrlimit(RLIMIT_NOFILE, &bounded) == 0, "cannot bound descriptor limit");
        }
        ~Limit() { (void)::setrlimit(RLIMIT_NOFILE, &saved); }
    } limit;
    LocalRefreshWorker worker;
    for (unsigned round = 0; round < 3; ++round) {
        for (const auto& request : projects) {
            const auto result = current(worker, request);
            check(result.at("observation").at("snapshot_current") == true,
                  "descriptor pressure bypassed source qualification");
        }
    }
    // Evicted optimization state must requalify real edits, not retain old proof.
    write(projects.front().root / "quota.cpp", "int quotaLimit() { return 909; }\n");
    const auto changed = worker.execute(payload(projects.front()), [] { return false; });
    check(!hit(changed), "resource eviction accepted stale qualification");
    (void)current(worker, projects.front());
#else
    (void)base; (void)profiles;
#endif
}
void image_digest_is_fenced(const fs::path& base) {
    for (const bool retain : {false, true}) {
        const auto binary = base / (retain ? "cached-image-digest-fixture" : "one-shot-image-digest-fixture");
        write(binary, "first-image");
        const auto original = qualification_image_digest(binary, retain);
        check(original == digest("first-image"), "executable digest did not match its content");
        const auto stamp = fs::last_write_time(binary);
        check(qualification_image_digest(binary, retain) == original, "unchanged executable digest drifted");
        write(binary, "other-image"); fs::last_write_time(binary, stamp);
        check(qualification_image_digest(binary, retain) == digest("other-image"), "restored-mtime image edit reused its hash");
        const auto replacement = base / "replacement-image";
        write(replacement, "first-image"); fs::last_write_time(replacement, stamp);
        fs::remove(binary); fs::rename(replacement, binary);
        check(qualification_image_digest(binary, retain) == original, "named image replacement retained an old digest");
    }
}
void canonical_authority(const fs::path& base, const fs::path& profiles) {
    namespace core = shin::context;
    LocalProjectRequest request; request.root = base / "canonical"; request.home = base / "canonical-home";
    const auto registry = base / "canonical-profiles", index = base / "canonical-index", state = base / "canonical-state";
    for (const auto& path : {request.root, request.home, registry, index, state}) ensure_private_directory(path, true);
    write(request.root / "README.md", "# Quota\n\nquotaLimit owns the request quota.\n");
    write(request.root / "quota.cpp", "int quotaLimit() { return 55; }\n");
    auto profile = parse_json(read_text(profiles / "generic-source.json", 1024 * 1024, false));
    profile["id"] = "idle-permanent"; profile["document"] = "shin-profile-idle-permanent";
    profile["rootFiles"] = Json::array({"README.md"});
    auto& optional = profile["optionalRootFiles"];
    optional.erase(std::remove(optional.begin(), optional.end(), "README.md"), optional.end());
    profile["bootstrapOnly"] = false;
    profile["evalCases"] = Json::array({{{"id", "quota-doc"}, {"query", "request quota"},
        {"mustPaths", Json::array({"README.md"})}}});
    profile["packEvalCases"] = Json::array({{{"id", "quota-pack"},
        {"mustPaths", Json::array({"README.md"})}, {"expectedComplete", true},
        {"request", {{"schema_version", "shin.task-request.v1"}, {"request_id", "quota-pack"},
            {"task", {{"objective", "Locate the request quota."},
                {"must", Json::array({{{"id", "rule"}, {"query", "request quota"}}})}}},
            {"budget", {{"max_evidence_bytes", 8192}, {"max_records", 4},
                {"per_query_records", 4}, {"per_document_records", 2}}}}}}});
    const auto profile_file = registry / "idle-permanent.json";
    check(write_private_archive(profile_file, profile.dump()), "canonical profile write failed");
    const Json config{{"schema_version", "shin.observer-config.v2"}, {"observer_id", "idle-agent"},
        {"source_host_id", "idle-host"}, {"project_id", "idle-project"}, {"workspace_id", "idle-workspace"},
        {"root", request.root.string()}, {"profile", "idle-permanent"}, {"profile_dir", registry.string()},
        {"out_dir", index.string()}, {"state_dir", state.string()}, {"poll_interval_ms", 100},
        {"max_snapshot_bytes", 1024 * 1024}, {"max_file_bytes", 256 * 1024},
        {"outbox", {{"enabled", false}, {"max_entries", 64}, {"max_bytes", 128 * 1024}}}};
    const auto config_file = base / "canonical-config.json";
    check(write_private_archive(config_file, config.dump()), "canonical configuration write failed");
    core::RuntimeOptions options; options.root = request.root; options.out_root = index; options.profile_dir = registry;
    options.profile = core::make_profile("idle-permanent", registry);
    check(core::run_build(options) == 0, "canonical build failed");
    (void)run_observer(config_from_json(config, config_file), 1, [] { return false; });
    request.command = "attach"; request.observer_config = config_file; request.manage_index = true;
    check(local_project(request).at("canonical_ready") == true, "canonical attachment failed");
    LocalRefreshWorker worker;
    const auto initial = warm(worker, request);
    check(initial.at("canonical_ready") == true, "canonical checkpoint lost readiness");
    const auto workspace = request.home / "projects" / digest(request.root.string());
    const auto receipt = workspace / "qualification/state.json", observer = state / "state.json";
    const auto saved_receipt = read_text(receipt, 256 * 1024, true), saved_observer = read_text(observer, 256 * 1024, true);
    const auto qualified_at = fs::last_write_time(receipt);
    check(hit(current(worker, request)) && read_text(receipt, 256 * 1024, true) == saved_receipt &&
        fs::last_write_time(receipt) == qualified_at, "idle check rewrote full qualification receipt");
    const auto source = request.root / "quota.cpp"; const auto stamp = fs::last_write_time(source);
    write(source, "int quotaLimit() { return 77; }\n"); fs::last_write_time(source, stamp);
    check(!hit(worker.execute(payload(request), [] { return false; })), "canonical same-metadata source edit was missed");
    const auto changed = warm(worker, request);
    check(changed.at("snapshot_id") != initial.at("snapshot_id"), "canonical source change was not published");
    const auto grant_file = workspace / "publication/state.json";
    auto grant = parse_json(read_text(grant_file, 256 * 1024, true));
    grant["pending_snapshot"] = grant.at("last_snapshot"); write(grant_file, grant.dump());
    check(!hit(current(worker, request)) &&
        parse_json(read_text(grant_file, 256 * 1024, true)).at("pending_snapshot") == "",
        "idle checkpoint bypassed pending publication reconciliation");
    (void)warm(worker, request);
    const auto baseline_file = workspace / "profile-revisions/state.json";
    const auto baseline = read_text(baseline_file, 256 * 1024, true);
    write(baseline_file, "{}");
    const auto invalid = worker.execute(payload(request), [] { return false; });
    check(!invalid.value("ok", false) && !hit(invalid) && invalid.at("error").at("code") == "profile-baseline-invalid",
        "idle checkpoint bypassed invalid approved-policy evidence");
    write(baseline_file, baseline); (void)warm(worker, request);
    const auto manifest = index / core::kGeneratedDir / "manifest.json";
    const auto original = read_text(manifest, 1024 * 1024, false), corrupt = "!" + original.substr(1);
    const auto manifest_stamp = fs::last_write_time(manifest);
    write(manifest, corrupt); fs::last_write_time(manifest, manifest_stamp);
    const auto corrupt_output = worker.execute(payload(request), [] { return false; });
    check(!hit(corrupt_output) && !corrupt_output.value("ok", false) &&
        corrupt_output.at("error").at("code") == "invalid-json" && !corrupt_output.contains("source_context"),
        "canonical manifest corruption lost its original refusal");
    write(manifest, original); (void)warm(worker, request);
    auto altered = profile; altered["sourceFactMaxFiles"] = profile.value("sourceFactMaxFiles", 1024) + 1;
    write(profile_file, altered.dump());
    const auto denied = worker.execute(payload(request), [] { return false; });
    check(!denied.value("ok", false) && denied.at("error").at("code") == "route-revision-conflict" && !hit(denied),
        "canonical checkpoint accepted a protected profile revision");
    write(profile_file, profile.dump()); (void)warm(worker, request);
    request.command = "refresh-release"; (void)local_project(request);
    const auto released = worker.execute(payload(request), [] { return false; });
    check(!hit(released) && released.value("refresh_authority", "") != "local-canonical", "revoked grant reused canonical authority");
    check(read_text(observer, 256 * 1024, true) == saved_observer, "canonical refresh mutated external Observer state");
}
void idle_and_scope_isolation(LocalRefreshWorker& worker, const LocalProjectRequest& first, const LocalProjectRequest& second) {
    const auto before = warm(worker, first);
    check(before.at("refresh_work").at("source_content_reverified") == false &&
        before.at("refresh_work").at("generated_content_reverified") == false, "idle path reverified corpus content");
    check(before.at("observation").at("content_reverified") == false && !before.at("rebuilt").get<bool>(), "idle path claimed a full audit");
    const auto other = warm(worker, second);
    check(other.at("index") != before.at("index"), "refresh scopes were mixed");
    const auto again = current(worker, first);
    check(hit(again) && again.at("observation").at("snapshot_id") == before.at("observation").at("snapshot_id"), "project switch lost its independent checkpoint");
}
void checkpoint_eviction(LocalRefreshWorker& worker, const LocalProjectRequest& request) {
    (void)warm(worker, request);
    for (unsigned scope = 0; scope < 16; ++scope) {
        auto unbound = payload(request);
        unbound["root"] = (request.root.parent_path() / ("unbound-" + std::to_string(scope))).string();
        check(!worker.execute(unbound, [] { return false; }).value("ok", false), "unbound scope was admitted");
    }
    check(!hit(current(worker, request)), "bounded scope retention failed to evict old checkpoint");
    check(hit(warm(worker, request)), "evicted scope could not fully requalify");
}
void response_budget(const LocalProjectRequest& request) {
    namespace core = shin::context;
    LocalRefreshWorker cold; auto full = current(cold, request);
    const auto workspace = request.home / "projects" / digest(request.root.string());
    core::RuntimeOptions options; options.root = request.root; options.out_root = workspace / "index";
    options.profile_dir = request.profile_dir.value(); options.profile = core::make_profile("generic-source", options.profile_dir);
    LocalRefreshSession session; Json unused;
    check(!session.reuse(options, workspace, "response-budget", unused), "unqualified checkpoint was reused");
    full["fixture_padding"] = "";
    const auto bytes = full.dump(2).size() + 1;
    check(bytes < 65510, "unexpected fixture response size");
    full["fixture_padding"] = std::string(65510 - bytes, 'x');
    check(full.dump(2).size() + 1 == 65510, "response-boundary fixture drifted");
    {
        const auto lock = core::lock_index_for_read(options.out_root);
        options.generation_admission = core::GenerationAdmission::reuse(options, core::generation_fingerprint(options));
        check(options.generation_admission != nullptr, "fixture generation could not be admitted");
        options.generation_admission->verify_content(options);
        session.remember(options, full);
    }
    check(!session.reuse(options, workspace, "response-budget", unused) && session.weight() == 0,
        "idle diagnostics exceeded the original response budget");
}
void persistent_process_and_suspend_boundary(const LocalProjectRequest& request, const fs::path& base) {
    double slept = 0;
    const auto executable = base / "refresh-worker.exe";
    fs::copy_file(current_executable_path(), executable);
#ifdef __APPLE__
    for (const auto& entry : fs::directory_iterator(current_executable_path().parent_path())) {
        if (entry.path().extension() == ".dylib")
            fs::copy_file(entry.path(), base / entry.path().filename());
    }
#endif
#ifdef _WIN32
    fixture_private_acl(executable);
#endif
    LocalWorkerProcess child(executable, LocalWorkerRole::refresh,
        [&] { return std::optional<SleepSample>(SleepSample{slept, slept}); });
    const auto pid = child.process_id();
    const auto deadline = [] { return std::chrono::steady_clock::now() + std::chrono::seconds(10); };
    for (int turn = 0; turn < 4; ++turn) {
        if (turn == 3) slept = 500; // Idle sleep is not interruption of an active check.
        child.send(payload(request), deadline());
        const auto result = child.receive(deadline());
        check(result.value("ok", false) && result.value("phase", "") == "current", "persistent refresh failed");
        // The copied executable can require its own first qualification receipt.
        // That write invalidates the initial native epoch; the next full pass
        // establishes the unchanged checkpoint rather than ignoring the write.
        if (turn >= 2) check(hit(result), "same refresh process lost its checkpoint");
        check(child.process_id() == pid, "refresh changed process identity");
    }
    child.send(payload(request), deadline()); slept = 1000;
    try { (void)child.receive(deadline()); }
    catch (const Error& error) {
        check(error.code == "refresh-suspended", "wrong suspended-refresh refusal");
        child.stop(); return;
    }
    throw std::runtime_error("active refresh accepted a suspend discontinuity");
}
void source_and_membership_changes(LocalRefreshWorker& worker, LocalProjectRequest request) {
    const auto before = warm(worker, request); const auto source = request.root / "quota.cpp";
    const auto stamp = fs::last_write_time(source);
    write(source, "int quotaLimit() { return 91; }\n"); fs::last_write_time(source, stamp);
    const auto changed = worker.execute(payload(request), [] { return false; });
    check(!hit(changed), "same-size restored-mtime edit reused qualification");
    const auto after = warm(worker, request);
    check(after.at("observation").at("snapshot_id") != before.at("observation").at("snapshot_id"), "source edit did not publish fresh content");
    request.command = "query"; request.query = "quotaLimit";
    check(local_project(request).at("source_context").get<std::string>().find("return 91") != std::string::npos, "edited source is stale");
    write(request.root / "added.cpp", "int freshLimit() { return 500; }\n");
    check(!hit(worker.execute(payload(request), [] { return false; })), "new source was missed");
    const auto added = warm(worker, request);
    fs::remove(request.root / "added.cpp");
    check(!hit(worker.execute(payload(request), [] { return false; })), "deleted source was missed");
    check(warm(worker, request).at("observation").at("snapshot_id") != added.at("observation").at("snapshot_id"), "deleted source remained published");
}
void authority_and_output_changes(LocalRefreshWorker& worker, const LocalProjectRequest& request) {
    (void)warm(worker, request);
    const auto workspace = request.home / "projects" / digest(request.root.string());
    const auto config = workspace / "config/state.json"; const auto saved = read_text(config, 65536, true);
    auto changed = parse_json(saved); changed["unauthorized_extra"] = true; write(config, changed.dump());
    const auto denied = worker.execute(payload(request), [] { return false; });
    check(!denied.value("ok", false) && !hit(denied), "changed authority reused idle admission");
    write(config, saved); (void)warm(worker, request);
    const auto manifest = workspace / "index" / shin::context::kGeneratedDir / "manifest.json";
    const auto original = read_text(manifest, 1024 * 1024, false); const auto stamp = fs::last_write_time(manifest);
    auto corrupt = original; corrupt[0] = '!'; write(manifest, corrupt); fs::last_write_time(manifest, stamp);
    const auto refused = worker.execute(payload(request), [] { return false; });
    check(!hit(refused) && (!refused.value("ok", false) || refused.value("rebuilt", false)), "corrupt output was admitted from metadata");
    write(manifest, original); (void)warm(worker, request);
}
void lost_monitor_and_cancellation(LocalRefreshWorker& worker, const LocalProjectRequest& request) {
    (void)warm(worker, request); const auto moved = request.root.parent_path() / "temporarily-moved";
    fs::rename(request.root, moved); std::this_thread::sleep_for(std::chrono::milliseconds(150)); fs::rename(moved, request.root);
    check(!hit(worker.execute(payload(request), [] { return false; })), "lost native observation reused admission");
    (void)warm(worker, request);
    const auto cancelled = worker.execute(payload(request), [] { return true; });
    check(!cancelled.value("ok", false) && cancelled.at("error").at("code") == "refresh-stopped", "idle reuse ignored cancellation");
    check(!hit(current(worker, request)), "cancelled checkpoint survived");
}
}
int main(int argc, char** argv) {
    std::string stage = "setup";
    try {
        if (argc == 2 && std::string_view(argv[1]) == "_refresh-worker") return shin::observer::local_refresh_worker();
        if (argc != 2 && argc != 3) return 2;
        Fixture fixture; const auto profiles = fs::canonical(fs::path(argv[1]));
        if (argc == 3 && std::string_view(argv[2]) == "--descriptor-pressure") {
            descriptor_pressure(fixture.path, profiles);
            return 0;
        }
        stage = "image digest";
        image_digest_is_fenced(fixture.path);
        stage = "project setup";
        const auto first = project(fixture.path, profiles, "first", 17), second = project(fixture.path, profiles, "second", 99);
        stage = "persistent process";
        persistent_process_and_suspend_boundary(first, fixture.path);
        LocalRefreshWorker worker;
        stage = "idle scopes";
        idle_and_scope_isolation(worker, first, second);
        stage = "checkpoint eviction";
        checkpoint_eviction(worker, first);
        stage = "response budget";
        response_budget(first);
        stage = "source changes";
        source_and_membership_changes(worker, first);
        stage = "authority and output";
        authority_and_output_changes(worker, first);
        stage = "canonical authority";
        canonical_authority(fixture.path, profiles);
        stage = "monitor and cancellation";
        lost_monitor_and_cancellation(worker, first);
        stage = "descriptor pressure";
        descriptor_pressure(fixture.path, profiles);
        std::cout << "idle reuse, source/membership, authority/output, monitor loss and cancellation passed\n";
    } catch (const Error& error) { std::cerr << stage << ": " << error.code << ": " << error.what() << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << stage << ": " << error.what() << '\n'; return 1; }
}
