#include "shin/observer/local_project.h"
#include "shin/observer/local_publication.h"
#include "shin/observer/local_telemetry.h"
#include "context_internal.h"
#include "worktree_state.h"
#include "source_read.h"
#include "../src/local_refresh_guard.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
using namespace shin::observer;
namespace core = shin::context;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Fixture {
    fs::path base, root, home, manifest;
    LocalProjectRequest request;
    explicit Fixture(const fs::path& profiles) {
        auto pattern = (fs::temp_directory_path() / "shin-refresh-race-XXXXXX").string();
        check(::mkdtemp(pattern.data()) != nullptr, "cannot create fixture");
        base = fs::canonical(pattern);
        root = base / "source"; home = base / "private";
        fs::create_directory(root);
        std::ofstream(root / "README.md") << "# Retry ceiling\n\nreadLimit owns the retry ceiling.\n";
        write(7);
        request.command = "init"; request.root = root; request.home = home; request.profile_dir = profiles;
        check(local_project(request).at("ok") == true, "fixture init failed");
        manifest = home / "projects" / digest(root.string()) / "index/docs/kb/generated/manifest.json";
        request.command = "watch";
    }
    ~Fixture() { std::error_code error; fs::remove_all(base, error); }
    void write(int value) {
        std::ofstream(root / "limits.js") << "export function readLimit() { return " << value << "; }\n";
    }
    core::RuntimeOptions options() const {
        core::RuntimeOptions value;
        if (!request.profile_dir) throw std::runtime_error("fixture profile directory is missing");
        value.root = root; value.profile_dir = request.profile_dir.value();
        value.profile = core::make_profile("generic-source", value.profile_dir);
        return value;
    }
};

void source_race(const fs::path& profiles, const std::string& mode) {
    Fixture fixture(profiles);
    const auto previous = core::read_file(fixture.manifest);
    unsigned calls = 0;
    bool saw_race = false, reset_token = false;
    LocalTelemetry telemetry;
    const auto refresh = [&](const LocalProjectRequest& request) -> Json {
        ++calls;
        if (calls == 2 && mode == "superseded") fixture.write(23);
        if ((calls == 2 && mode != "superseded") || (calls == 3 && mode == "superseded")) {
            auto options = fixture.options();
            const auto inventory = core::observe_worktree(options);
            fixture.write(311);
            if (mode == "stale-index" || mode == "qualification") {
                prepare_with_source_fence([] {}, [&] { core::require_worktree_stable(options, inventory); }, [&] {
                    if (mode == "stale-index") core::throw_index_error("stale-index", "stale generation");
                    throw Error("qualification-inputs-changed", "qualification drift");
                });
                throw std::runtime_error("qualification race was not rejected");
            }
            if (mode == "source-binding") {
                prepare_with_source_fence([] {}, [&] { core::require_worktree_stable(options, inventory); }, [&] {
                    (void)core::read_bound_source(options.root, "limits.js", inventory.source_facts.at("limits.js").content_hash, 4096);
                });
                throw std::runtime_error("changed source binding was not rejected");
            }
            if (mode == "superseded")
                throw Error("refresh-superseded", "Source changed after the coalesced observation.");
            try { core::require_worktree_stable(options, inventory); }
            catch (const core::CliError& error) {
                if (mode == "worker") throw Error(error.code(), error.what());
                throw;
            }
            throw std::runtime_error("source race was not detected");
        }
        if (saw_race && !reset_token) {
            check(request.maintenance_worktree.empty(), "retired observation token was reused");
            reset_token = true;
        }
        return local_project(request);
    };
    const auto report = [&](const fs::path& root, const Json& state) {
        telemetry.record(root, state);
        const auto& last = state.at("last_result");
        if (state.at("phase") == "checking") return;
        if (last.is_object() && last.contains("error")) {
            const auto code = last.at("error").value("code", "");
            check(code != "file-changed-during-verification" && code != "refresh-superseded" && code != "source-read-hash-mismatch",
                  "source race became a blocking failure");
        }
        if (!last.is_object() || !last.contains("retry_reason")) return;
        saw_race = true;
        check(state.at("phase") == "coalescing", "source race became a blocking failure");
        check(state.at("failures") == 0 && state.at("superseded_refreshes") == 1, "source race inflated failure counters");
        check(last.at("ok") == false && !last.contains("error"), "source race advertised readiness or a hard error");
        check(core::read_file(fixture.manifest) == previous, "source race replaced the preceding index");
        auto query = fixture.request;
        query.command = "query"; query.query = "readLimit";
        const auto stale = local_project(query);
        check(stale.at("ok") == false && !stale.contains("source_context"), "source race leaked stale context");
        const auto project = telemetry.snapshot().at("project_states").at(0);
        check(project.at("active_issue") == false && project.at("error_code") == "", "source race became a UI blocker");
    };
    const auto result = watch_local_projects(fixture.request, {fixture.root}, mode == "superseded" ? 4 : 3,
        [] { return false; }, {}, {}, report, refresh);
    const auto& state = result.at("watch");
    check(saw_race && reset_token, "race/reset path was not covered");
    check(state.at("last_result").at("phase") == "current" && state.at("publications") == 1 &&
          state.at("failures") == 0, "source race did not recover normally");
    fixture.request.command = "query"; fixture.request.query = "readLimit";
    const auto query = local_project(fixture.request);
    check(query.at("ok") == true && query.at("source_context").get<std::string>().find("311") != std::string::npos,
          "recovered context is not current");
}

void hard_failure(const fs::path& profiles, const std::string& code) {
    Fixture fixture(profiles);
    unsigned calls = 0;
    const auto refresh = [&](const LocalProjectRequest& request) -> Json {
        if (++calls == 1) return local_project(request);
        throw Error(code, "permanent failure fixture");
    };
    LocalTelemetry telemetry;
    const auto report = [&](const fs::path& root, const Json& state) {
        telemetry.record(root, state);
        if (state.at("phase") == "checking") return;
        check(state.at("phase") == "blocked" && telemetry.snapshot().at("project_states").at(0).at("active_issue") == true,
              "real failure lost its blocking status");
    };
    const auto result = watch_local_projects(fixture.request, {fixture.root}, 1,
        [] { return false; }, {}, {}, report, refresh);
    const auto& state = result.at("watch");
    check(state.at("failures") == 1 && state.at("last_result").at("error").at("code") == code,
          "real failure was weakened");
}

Json failed_observation(const std::string& code, const std::string& status = "error") {
    Json observation{{"status", status}, {"reason_code", code}};
    if (status != "error") return observation;
    const auto category = code == "file-changed-during-verification" ? "entry-disappeared" :
        code == "access-denied" ? "access-denied" : "unexpected-error";
    observation["diagnostic"] = {{"schema_version", "shin.source-observation-diagnostic.v1"},
            {"stage", "source-stability"}, {"operation", "verify-source-metadata"},
            {"category", category}};
    return observation;
}

void returned_observation_race_recovers(const fs::path& profiles) {
    Fixture fixture(profiles);
    const auto previous = core::read_file(fixture.manifest);
    unsigned calls = 0;
    bool saw_error = false, cleared_token = false;
    const auto refresh = [&](const LocalProjectRequest& request) -> Json {
        ++calls;
        if (calls == 1) return local_project(request);
        if (calls == 2) {
            return Json{{"ok", true}, {"phase", "coalescing"}, {"rebuilt", false},
                {"refresh_authority", "private-bootstrap"},
                {"workspace_identity", request.maintenance_workspace_identity},
                {"observation", {{"status", "stale"}, {"worktree_id", "stale-observation-token"},
                    {"snapshot_current", false}}}};
        }
        if (calls == 3) {
            check(request.maintenance_worktree == "stale-observation-token", "fixture did not seed a stale token");
            fixture.write(311);
            return Json{{"ok", false}, {"phase", "blocked"}, {"rebuilt", false},
                {"refresh_authority", "private-bootstrap"},
                {"workspace_identity", request.maintenance_workspace_identity},
                {"observation", failed_observation("file-changed-during-verification")}};
        }
        if (calls == 4) {
            check(request.maintenance_worktree.empty(), "failed observation reused a stale token");
            cleared_token = true;
        }
        return local_project(request);
    };
    LocalTelemetry telemetry;
    const auto report = [&](const fs::path& root, const Json& state) {
        telemetry.record(root, state);
        if (state.at("phase") == "checking") return;
        if (!state.at("last_result").is_object() ||
            state.at("last_result").value("retry_reason", "") != "file-changed-during-verification") return;
        saw_error = true;
        check(state.at("phase") == "coalescing" && state.at("failures") == 0 &&
              state.at("superseded_refreshes") == 1, "returned source observation did not coalesce");
        check(state.at("last_result").at("diagnostic") ==
              failed_observation("file-changed-during-verification").at("diagnostic"),
              "transient observation diagnostic was not retained");
        check(state.at("last_result").at("observation") ==
              failed_observation("file-changed-during-verification"),
              "transient failed observation envelope was discarded");
        check(state.at("publications") == 0 && core::read_file(fixture.manifest) == previous,
              "failed observation published over the preceding index");
        const auto project = telemetry.snapshot().at("project_states").at(0);
        check(project.at("active_issue") == false && project.at("error_code") == "",
              "transient observation became a telemetry blocker");
        auto query = fixture.request;
        query.command = "query"; query.query = "readLimit";
        const auto stale = local_project(query);
        check(stale.at("ok") == false && !stale.contains("source_context"),
              "failed observation served stale source context");
    };
    const auto result = watch_local_projects(fixture.request, {fixture.root}, 4,
        [] { return false; }, {}, {}, report, refresh);
    const auto& state = result.at("watch");
    if (!saw_error || !cleared_token || calls != 5)
        throw std::runtime_error("returned observation retry/recovery path was not completed: saw_error=" +
            std::to_string(saw_error) + ", cleared_token=" + std::to_string(cleared_token) +
            ", calls=" + std::to_string(calls) + ", state=" + state.dump());
    check(state.at("publications") == 1 && state.at("failures") == 0 &&
          state.at("last_result").at("phase") == "current",
          "returned observation failure did not recover on the fresh source token");
    fixture.request.command = "query"; fixture.request.query = "readLimit";
    const auto current = local_project(fixture.request);
    check(current.at("ok") == true && current.at("source_context").get<std::string>().find("311") != std::string::npos,
          "recovered query did not use the refreshed source");
}

void returned_observation_remains_blocked(const fs::path& profiles, const std::string& code,
                                          bool include_error, const std::string& status = "error") {
    Fixture fixture(profiles);
    const auto previous = core::read_file(fixture.manifest);
    unsigned calls = 0;
    const auto observation = failed_observation(code, status);
    const auto refresh = [&](const LocalProjectRequest& request) -> Json {
        if (++calls == 1) return local_project(request);
        Json tick{{"ok", false}, {"phase", "blocked"}, {"rebuilt", false},
            {"refresh_authority", include_error ? "local-canonical" : "private-bootstrap"},
            {"workspace_identity", request.maintenance_workspace_identity}, {"observation", observation}};
        // Older private-bootstrap envelopes exposed the typed observation reason
        // without a top-level error; managed refresh envelopes already include one.
        if (include_error) tick["error"] = error_json(Error(code, "Source observation failed; previous index preserved.",
                                                               observation.value("diagnostic", Json())));
        return tick;
    };
    Json sampled;
    const auto report = [&](const fs::path&, const Json& state) {
        if (state.at("phase") != "checking" && state.at("ticks") != 0) sampled = state;
    };
    const auto result = watch_local_projects(fixture.request, {fixture.root}, 1,
        [] { return false; }, {}, {}, report, refresh);
    const auto& state = result.at("watch");
    check(!sampled.is_null() && sampled.at("phase") == "blocked" && sampled.at("failures") == 1,
          "permanent source observation error did not remain blocked");
    check(sampled.at("last_result").at("phase") == "blocked",
          "blocked observation metadata lost its controller phase");
    check(sampled.at("last_result").at("error").at("code") == code,
          "typed source observation error code was lost");
    check(sampled.at("last_result").at("observation") == observation,
          "failed observation envelope was discarded");
    if (observation.contains("diagnostic"))
        check(sampled.at("last_result").at("error").at("diagnostic") == observation.at("diagnostic"),
              "source observation diagnostic was not retained");
    else
        check(!sampled.at("last_result").at("error").contains("diagnostic"),
              "unexpected diagnostic appeared for an incomplete observation");
    check(state.at("publications") == 0 && core::read_file(fixture.manifest) == previous,
          "blocked source observation changed the prior index");
}

void returned_observation_preserves_conflicting_error(const fs::path& profiles) {
    Fixture fixture(profiles);
    const auto previous = core::read_file(fixture.manifest);
    unsigned calls = 0;
    const auto refresh = [&](const LocalProjectRequest& request) -> Json {
        if (++calls == 1) return local_project(request);
        return Json{{"ok", false}, {"phase", "blocked"}, {"rebuilt", false},
            {"refresh_authority", "local-canonical"},
            {"workspace_identity", request.maintenance_workspace_identity},
            {"observation", failed_observation("file-changed-during-verification")},
            {"error", {{"code", "binding-changed"}, {"message", "Authority changed during refresh."}}}};
    };
    Json sampled;
    const auto report = [&](const fs::path&, const Json& state) {
        if (state.at("phase") != "checking" && state.at("ticks") != 0) sampled = state;
    };
    const auto result = watch_local_projects(fixture.request, {fixture.root}, 1,
        [] { return false; }, {}, {}, report, refresh);
    const auto& state = result.at("watch");
    check(!sampled.is_null() && sampled.at("phase") == "blocked" && sampled.at("failures") == 1,
          "conflicting authority error was classified as a source race");
    check(sampled.at("last_result").at("error").at("code") == "binding-changed" &&
          !sampled.at("last_result").contains("retry_reason") && sampled.at("superseded_refreshes") == 0,
          "top-level authority error was replaced by observation reason");
    check(state.at("publications") == 0 && core::read_file(fixture.manifest) == previous,
          "conflicting authority error changed the prior index");
}

void unchanged_binding_failure(const fs::path& profiles, bool revoke) {
    Fixture fixture(profiles);
    const auto old_hash = core::fnv1a64(core::read_file(fixture.root / "limits.js"));
    fixture.write(311);
    auto options = fixture.options();
    const auto inventory = core::observe_worktree(options);
    bool checked_authority = false;
    try {
        prepare_with_source_fence([&] {
            checked_authority = true;
            if (revoke) throw Error("refresh-root-changed", "authority fixture");
        }, [&] { core::require_worktree_stable(options, inventory); },
        [&] { (void)core::read_bound_source(options.root, "limits.js", old_hash, 4096); });
        throw std::runtime_error("stale binding accepted");
    } catch (const core::CliError& error) {
        check(!revoke && checked_authority && error.code() == "source-read-hash-mismatch",
              "unchanged binding failure was hidden");
    } catch (const Error& error) {
        check(revoke && checked_authority && error.code == "refresh-root-changed",
              "authority failure lost precedence");
    }
}
void unchanged_qualification_failure(const fs::path& profiles, bool revoke, bool stale) {
    Fixture fixture(profiles);
    auto options = fixture.options();
    const auto inventory = core::observe_worktree(options);
    try {
        prepare_with_source_fence([&] {
            if (revoke) throw Error("refresh-authority-changed", "revoked fixture");
        }, [&] { core::require_worktree_stable(options, inventory); }, [&] {
            if (stale) core::throw_index_error("stale-index", "unchanged fixture");
            throw Error("qualification-inputs-changed", "unchanged fixture");
        });
        throw std::runtime_error("failure disappeared");
    } catch (const core::CliError& error) {
        check(!revoke && stale && error.code() == "stale-index", "unchanged stale failure was hidden");
    } catch (const Error& error) {
        check(error.code == (revoke ? "refresh-authority-changed" : "qualification-inputs-changed"),
              "unchanged qualification or authority failure was hidden");
    }
}

void config_revision_during_final_verification(const fs::path& profiles) {
    Fixture fixture(profiles);
    const auto external_home = fixture.base / "external-home";
    const auto external_index = fixture.base / "external-index";
    const auto external_state = fixture.base / "external-state";
    const auto config_file = fixture.base / "observer-config.json";
    const auto permanent_profiles = fixture.base / "permanent-profiles";
    ensure_private_directory(external_home, true);
    ensure_private_directory(external_index, true);
    ensure_private_directory(external_state, true);
    ensure_private_directory(permanent_profiles, true);
    auto profile = core::parse_json_text(core::read_file(profiles / "generic-source.json"),
                                         "neutral profile fixture");
    profile["document"] = "shin-profile-race-permanent";
    profile["id"] = "race-permanent";
    profile["summary"] = "Neutral permanent profile for route revision race coverage.";
    profile["rootFiles"] = Json::array({"README.md"});
    auto optional_root_files = profile.at("optionalRootFiles");
    optional_root_files.erase(std::remove(optional_root_files.begin(), optional_root_files.end(), "README.md"),
                              optional_root_files.end());
    profile["optionalRootFiles"] = std::move(optional_root_files);
    profile["bootstrapOnly"] = false;
    profile["evalCases"] = Json::array({{{"id", "route-doc"}, {"query", "retry ceiling"},
        {"mustPaths", Json::array({"README.md"})}}});
    profile["packEvalCases"] = Json::array({{{"id", "route-pack"},
        {"mustPaths", Json::array({"README.md"})}, {"expectedComplete", true},
        {"request", {{"schema_version", "shin.task-request.v1"}, {"request_id", "route-pack"},
            {"task", {{"objective", "Locate the retry ceiling."},
                {"must", Json::array({{{"id", "rule"}, {"query", "retry ceiling"}}})}}},
            {"budget", {{"max_evidence_bytes", 8192}, {"max_records", 4},
                {"per_query_records", 4}, {"per_document_records", 2}}}}}}});
    check(write_private_archive(permanent_profiles / "race-permanent.json", profile.dump(2) + "\n"),
          "permanent profile fixture write failed");
    const Json config = {
        {"schema_version", "shin.observer-config.v2"},
        {"observer_id", "observer-race-agent"},
        {"source_host_id", "observer-race-host"},
        {"project_id", "observer-race-project"},
        {"workspace_id", "observer-race-workspace"},
        {"root", fixture.root.string()},
        {"profile", "race-permanent"},
        {"profile_dir", permanent_profiles.string()},
        {"out_dir", external_index.string()},
        {"state_dir", external_state.string()},
        {"poll_interval_ms", 100},
        {"max_snapshot_bytes", 1024 * 1024},
        {"max_file_bytes", 256 * 1024},
        {"outbox", {{"enabled", false}, {"max_entries", 64}, {"max_bytes", 128 * 1024}}}};
    check(write_private_archive(config_file, config.dump(2) + "\n"),
          "external route config fixture write failed");

    auto options = fixture.options();
    options.out_root = external_index;
    options.profile_dir = permanent_profiles;
    options.profile = core::make_profile("race-permanent", permanent_profiles);
    check(core::run_build(options) == 0, "external route fixture build failed");
    const auto observer_state = run_observer(config_from_json(config, config_file), 1,
                                             [] { return false; });
    check(observer_state.at("sequence") == 1 && !observer_state.at("observation").is_null(),
          "external route fixture Observer sample failed");

    LocalProjectRequest attached;
    attached.command = "attach";
    attached.root = fixture.root;
    attached.home = external_home;
    attached.profile_dir = permanent_profiles;
    attached.observer_config = config_file;
    const auto attach_result = local_project(attached);
    if (attach_result.at("ok") != true)
        throw std::runtime_error("external route fixture attach failed: " + attach_result.dump());

    auto query = attached;
    query.command = "_refresh";
    bool changed_at_final_verification = false;
    query.maintenance_progress = [&](const std::string& stage) {
        if (stage != "final-verification" || changed_at_final_verification) return;
        changed_at_final_verification = true;
        std::ofstream(config_file, std::ios::app) << '\n';
    };
    try {
        (void)local_project(query);
        throw std::runtime_error("configuration revision race was accepted");
    } catch (const Error& error) {
        check(changed_at_final_verification, "config mutation did not run at final verification");
        check(error.code == "route-revision-conflict",
              "configuration revision changed at final verification lost route-revision-conflict classification");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "profile registry argument required");
        constexpr std::uintmax_t gib = 1024ULL*1024*1024;
        verify_storage_capacity(7*gib,gib,3*gib);
        for (const auto& failure : {std::pair{8*gib,"storage-quota-exceeded"},std::pair{std::uintmax_t{0},"storage-free-space-low"}}) {
            try { verify_storage_capacity(failure.first,gib,2*gib); throw std::runtime_error("unsafe allocation admitted"); }
            catch (const Error& error) { check(error.code==failure.second,"wrong storage failure classification"); }
        }
        const auto profiles = fs::canonical(argv[1]);
        for (const auto* mode : {"core", "worker", "superseded", "source-binding", "stale-index", "qualification"}) source_race(profiles, mode);
        returned_observation_race_recovers(profiles);
        returned_observation_remains_blocked(profiles, "source-observation-failed", true);
        returned_observation_remains_blocked(profiles, "access-denied", false);
        returned_observation_remains_blocked(profiles, "incomplete-source-observation", true, "incomplete");
        returned_observation_preserves_conflicting_error(profiles);
        for (bool revoke : {false, true}) for (bool stale : {false, true})
            unchanged_qualification_failure(profiles, revoke, stale);
        unchanged_binding_failure(profiles, false);
        unchanged_binding_failure(profiles, true);
        for (const auto* code : {"refresh-validation-failed", "refresh-output-budget", "refresh-root-changed",
                                "route-revision-conflict", "qualification-inputs-changed", "refresh-base-changed",
                                "generation-admission-changed", "file-changed-during-verification-other", "source-read-hash-mismatch"})
            hard_failure(profiles, code);
        config_revision_during_final_verification(profiles);
        std::cout << "source races retry; real failures remain blocked\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
