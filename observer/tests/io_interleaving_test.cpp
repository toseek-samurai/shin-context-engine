#include "shin/observer/io.h"
#include "shin/observer/local_project.h"
#include "shin/observer/local_registry.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

extern "C" int shin_observer_test_openat(int directory, const char* name, int flags, ...);
extern "C" int shin_observer_test_fstatat(int directory, const char* name, struct stat* status, int flags);

namespace {
using namespace shin::observer;
namespace fs = std::filesystem;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Action>
void rejects(const char* code, Action action) {
    try {
        action();
    } catch (const Error& error) {
        if (error.code != code) {
            throw std::runtime_error(std::string("expected ") + code + ", got " + error.code);
        }
        return;
    }
    throw std::runtime_error(std::string("expected rejection: ") + code);
}

class Fixture {
public:
    Fixture() {
        std::string pattern = (fs::temp_directory_path() / "shin-observer-interleaving-XXXXXX").string();
        check(::mkdtemp(pattern.data()) != nullptr, "fixture root failed");
        root = fs::canonical(pattern);
        check(::chmod(root.c_str(), 0700) == 0, "fixture root chmod failed");
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    fs::path root;
};

struct Interleaving {
    StateStore& writer;
    unsigned replacement_limit;
    mode_t replacement_mode = 0600;
    unsigned attempts = 0;
    unsigned replacements = 0;

    void after_open(int directory, const char* name, int descriptor) {
        ++attempts;
        check(attempts <= 3, "state load exceeded three attempts");
        if (replacements == replacement_limit) return;
        ++replacements;
        writer.save(Json{{"generation", replacements}, {"payload", "replacement"}});
        struct stat old_status {};
        check(::fstat(descriptor, &old_status) == 0 && old_status.st_nlink == 0,
              "writer did not unlink the already-open state inode");
        if (replacement_mode != 0600) {
            check(::fchmodat(directory, name, replacement_mode, 0) == 0, "replacement chmod failed");
        }
    }
};

struct NamedInterleaving {
    StateStore& writer;
    unsigned limit;
    mode_t replacement_mode = 0600;
    unsigned observations = 0, replacements = 0;
    bool inside_writer = false;
};
NamedInterleaving* named_interleaving = nullptr;
class ScopedNamedInterleaving {
public:
    explicit ScopedNamedInterleaving(NamedInterleaving& value) { named_interleaving = &value; }
    ~ScopedNamedInterleaving() { named_interleaving = nullptr; }
    ScopedNamedInterleaving(const ScopedNamedInterleaving&) = delete;
    ScopedNamedInterleaving& operator=(const ScopedNamedInterleaving&) = delete;
};

void initial_stat_replacement_retries_boundedly() {
    for (const unsigned limit : {1U, 3U}) {
        Fixture fixture;
        StateStore writer(fixture.root, true);
        writer.save(Json{{"generation", 0}});
        StateStore reader(fixture.root, false);
        NamedInterleaving interleaving{writer, limit};
        const ScopedNamedInterleaving scope(interleaving);
        if (limit == 1) check(reader.load() == Json{{"generation", 1}}, "initial stat race did not return the new state");
        else rejects("state-changed", [&] { (void)reader.load(); });
        check(interleaving.replacements == limit, "initial stat replacement bypassed bounded retry");
    }
}
void initial_stat_race_does_not_accept_unsafe_replacement() {
    for (const mode_t mode : {0644, 0666}) {
        Fixture fixture;
        StateStore writer(fixture.root, true);
        writer.save(Json{{"generation", 0}});
        StateStore reader(fixture.root, false);
        NamedInterleaving interleaving{writer, 1, mode};
        const ScopedNamedInterleaving scope(interleaving);
        rejects("state-unsafe", [&] { (void)reader.load(); });
        check(interleaving.observations == 4 && interleaving.replacements == 1,
            "unsafe replacement was not rejected on the second attempt");
    }
}

Interleaving* active_interleaving = nullptr;
int next_pending_error = 0;

class ScopedInterleaving {
public:
    explicit ScopedInterleaving(Interleaving& interleaving) {
        check(active_interleaving == nullptr, "nested interleaving fixture");
        active_interleaving = &interleaving;
    }
    ~ScopedInterleaving() { active_interleaving = nullptr; }
    ScopedInterleaving(const ScopedInterleaving&) = delete;
    ScopedInterleaving& operator=(const ScopedInterleaving&) = delete;
};

void one_replacement_returns_new_json() {
    Fixture fixture;
    StateStore writer(fixture.root, true);
    writer.save(Json{{"generation", 0}, {"payload", "original"}});
    StateStore reader(fixture.root, false);
    Interleaving interleaving{writer, 1};
    const ScopedInterleaving scope(interleaving);

    const auto loaded = reader.load();
    check(loaded.has_value() && *loaded == Json{{"generation", 1}, {"payload", "replacement"}},
          "load returned absent, stale, or incomplete replacement JSON");
    check(interleaving.attempts == 2 && interleaving.replacements == 1,
          "one replacement must succeed on the second attempt");
    check(!fs::exists(fixture.root / "state.pending"), "replacement left pending state");
}

void repeated_replacement_stops_after_three_attempts() {
    Fixture fixture;
    StateStore writer(fixture.root, true);
    writer.save(Json{{"generation", 0}});
    StateStore reader(fixture.root, false);
    Interleaving interleaving{writer, 3};
    const ScopedInterleaving scope(interleaving);

    rejects("state-changed", [&] { (void)reader.load(); });
    check(interleaving.attempts == 3 && interleaving.replacements == 3,
          "repeated replacement must exhaust exactly three attempts");
}

constexpr mode_t kUnsafeModes[] = {0400, 0644, 0660, 0666, 01600};

void unsafe_replacement_is_not_bypassed() {
    for (const mode_t mode : kUnsafeModes) {
        Fixture fixture;
        StateStore writer(fixture.root, true);
        writer.save(Json{{"generation", 0}});
        StateStore reader(fixture.root, false);
        Interleaving interleaving{writer, 1, mode};
        const ScopedInterleaving scope(interleaving);

        rejects("state-unsafe", [&] { (void)reader.load(); });
        check(interleaving.attempts == 1 && interleaving.replacements == 1,
              "unsafe replacement was reopened or never injected");
        struct stat status {};
        check(::stat((fixture.root / "state.json").c_str(), &status) == 0 &&
                  (status.st_mode & 07777) == mode,
              "reader repaired unsafe replacement permissions");
    }
}

void initial_unsafe_modes_are_not_retried() {
    for (const mode_t mode : kUnsafeModes) {
        Fixture fixture;
        StateStore writer(fixture.root, true);
        writer.save(Json{{"generation", 0}});
        check(::chmod((fixture.root / "state.json").c_str(), mode) == 0, "initial state chmod failed");
        StateStore reader(fixture.root, false);
        Interleaving interleaving{writer, 0};
        const ScopedInterleaving scope(interleaving);

        rejects("state-unsafe", [&] { (void)reader.load(); });
        check(interleaving.attempts == 0 && interleaving.replacements == 0,
              "initial unsafe state reached openat");
    }
}

} // namespace

extern "C" int shin_observer_test_fstatat(int directory, const char* name, struct stat* status, int flags) {
    const int result = ::fstatat(directory, name, status, flags);
    auto* interleaving = named_interleaving;
    if (result != 0 || !interleaving || interleaving->inside_writer || std::strcmp(name, "state.json") != 0) return result;
    // Each attempt first checks existence, then captures the named file's stamp.
    ++interleaving->observations;
    if (interleaving->observations % 2 != 0 || interleaving->replacements == interleaving->limit) return result;
    const int descriptor = ::openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    check(descriptor >= 0, "cannot hold the initially resolved inode");
    interleaving->inside_writer = true;
    try {
        interleaving->writer.save(Json{{"generation", ++interleaving->replacements}});
        if (interleaving->replacement_mode != 0600)
            check(::fchmodat(directory, name, interleaving->replacement_mode, 0) == 0, "replacement chmod failed");
        // Linux can resolve the old inode, then observe its attributes after rename.
        check(::fstat(descriptor, status) == 0 && status->st_nlink == 0, "initial stat did not observe the unlinked inode");
    } catch (...) { interleaving->inside_writer = false; ::close(descriptor); throw; }
    interleaving->inside_writer = false;
    ::close(descriptor);
    return result;
}

extern "C" int shin_observer_test_openat(int directory, const char* name, int flags, ...) {
    if (next_pending_error && (flags & O_CREAT) && std::strcmp(name, "state.pending") == 0) {
        errno = std::exchange(next_pending_error, 0);
        return -1;
    }
    int descriptor = -1;
    bool needs_mode = (flags & O_CREAT) != 0;
#if defined(O_TMPFILE)
    needs_mode = needs_mode || (flags & O_TMPFILE) == O_TMPFILE;
#endif
    if (needs_mode) {
        va_list arguments;
        va_start(arguments, flags);
        const auto mode = static_cast<mode_t>(va_arg(arguments, unsigned int));
        va_end(arguments);
        descriptor = ::openat(directory, name, flags, mode);
    } else {
        descriptor = ::openat(directory, name, flags);
    }
    if (descriptor >= 0 && active_interleaving != nullptr && (flags & O_ACCMODE) == O_RDONLY &&
        std::strcmp(name, "state.json") == 0) {
        // The real writer replaces the name while the reader still owns the old inode.
        try {
            active_interleaving->after_open(directory, name, descriptor);
        } catch (...) {
            ::close(descriptor);
            throw;
        }
    }
    return descriptor;
}

namespace {
#ifdef SHIN_TEST_LOCAL_CONTROLLER
void changing_watch_observation_does_not_replace_authority() {
    Fixture fixture;
    StateStore writer(fixture.root, true);
    writer.save(Json{{"generation", 0}});
    Interleaving interleaving{writer, 3};
    const ScopedInterleaving scope(interleaving);
    const auto status = local_watch_status(fixture.root);
    check(status.at("running") == true && status.at("state").is_null() &&
        status.at("state_error_code") == "state-changed", "watch race was hidden or aborted a read");
    check(interleaving.attempts == 3 && interleaving.replacements == 3,
        "watch race bypassed bounded state verification");
}

void unsafe_watch_observation_is_not_ignored() {
    for (const auto mode : kUnsafeModes) {
        Fixture fixture;
        StateStore writer(fixture.root, true);
        writer.save(Json{{"generation", 0}});
        Interleaving interleaving{writer, 1, mode};
        const ScopedInterleaving scope(interleaving);
        rejects("state-unsafe", [&] { (void)local_watch_status(fixture.root); });
    }
}

Json prepare_lifecycle_binding(const fs::path& root, const fs::path& workspace) {
    ensure_private_directory(workspace);
    const Json binding{{"root", root.string()}, {"root_identity", directory_identity(root)},
        {"workspace_identity", directory_identity(workspace, true)}, {"prepared_snapshot", "preserved"}};
    StateStore writer(workspace, true);
    writer.save(binding);
    return binding;
}

void incomplete_lifecycle_intent_is_fenced_and_revisions_survive_reactivation() {
    Fixture fixture;
    const auto root = fixture.root / "source", home = fixture.root / "home";
    fs::create_directory(root);
    ensure_private_directory(home);
    ensure_private_directory(home / "projects");
    const auto workspace = home / "projects" / digest(root.string());
    const auto binding = prepare_lifecycle_binding(root, workspace);
    StateStore binding_writer(workspace, true);
    const auto admitted = require_local_project_active(workspace);
    check(admitted == LocalProjectLifecycle{"active", 0}, "legacy lifecycle was not active revision zero");

    next_pending_error = ENOSPC;
    rejects("storage-full", [&] { set_local_project_lifecycle(workspace, binding, "detaching"); });
    check(next_pending_error == 0, "first lifecycle write did not consume the storage failure");
    check(local_project_lifecycle(workspace) == LocalProjectLifecycle{"detaching", 0},
          "incomplete first intent was not fenced");
    rejects("project-detached", [&] { (void)require_local_project_active(workspace); });

    set_local_project_lifecycle(workspace, binding, "detaching");
    check(local_project_lifecycle(workspace) == LocalProjectLifecycle{"detaching", 1},
          "retry did not commit the first lifecycle revision");
    set_local_project_lifecycle(workspace, binding, "detached");
    check(local_project_lifecycle(workspace) == LocalProjectLifecycle{"detached", 2},
          "detach completion did not advance the revision");
    rejects("project-detached", [&] { (void)require_local_project_active(workspace); });
    set_local_project_lifecycle(workspace, binding, "active");
    const auto reactivated = require_local_project_active(workspace);
    check(reactivated == LocalProjectLifecycle{"active", 3} && reactivated != admitted,
          "reactivation resurrected the old admission revision");
    check(binding_writer.load() == std::optional<Json>(binding), "lifecycle changed the prepared binding");
}

void watch_admission_rejects_detach_and_reactivation_during_refresh() {
    for (const bool reactivate : {false, true}) {
        Fixture fixture;
        const auto root = fixture.root / "source", home = fixture.root / "home";
        fs::create_directory(root);
        ensure_private_directory(home);
        ensure_private_directory(home / "projects");
        const auto workspace = home / "projects" / digest(root.string());
        const auto binding = prepare_lifecycle_binding(root, workspace);
        unsigned refreshes = 0;
        LocalProjectRequest request;
        request.root = root;
        request.home = home;

        rejects("project-detached", [&] {
            (void)watch_local_projects(request, {root}, 1,
                [] { return false; }, {}, {}, {}, [&](const LocalProjectRequest&) {
                    ++refreshes;
                    StateStore binding_writer(workspace, true);
                    set_local_project_lifecycle(workspace, binding, "detaching");
                    set_local_project_lifecycle(workspace, binding, "detached");
                    if (reactivate) set_local_project_lifecycle(workspace, binding, "active");
                    return Json{{"ok", true}, {"phase", "current"}, {"rebuilt", false},
                        {"refresh_authority", "private-bootstrap"}, {"workspace_identity", binding.at("workspace_identity")}};
                });
        });
        check(refreshes == 1, "stale watch admission retried refresh");
        check(!fs::exists(workspace / "watch"), "stale admission installed a controller");
        StateStore reader(workspace, false);
        check(!reader.writer_present() && reader.load() == std::optional<Json>(binding),
              "rejected admission retained a writer or changed the binding");
    }
}

void retirement_callback_failures_preserve_the_sibling_controller() {
    Fixture fixture;
    const auto root = fixture.root / "source", sibling = fixture.root / "sibling", home = fixture.root / "home";
    fs::create_directory(root);
    fs::create_directory(sibling);
    ensure_private_directory(home);
    ensure_private_directory(home / "projects");
    const auto workspace = home / "projects" / digest(root.string());
    const auto sibling_workspace = home / "projects" / digest(sibling.string());
    const auto binding = prepare_lifecycle_binding(root, workspace);
    const auto sibling_binding = prepare_lifecycle_binding(sibling, sibling_workspace);
    bool fenced = false;
    unsigned removals = 0, diagnostics = 0;
    LocalEnrollmentHooks enrollment;
    enrollment.removed = [&](const fs::path& removed) {
        check(removed == root, "retirement removed the sibling");
        ++removals;
        throw std::runtime_error("retirement callback failure");
    };
    enrollment.report = [&](const LocalProjectRequest& request, const std::string& error) {
        check(request.root == root && error == "lifecycle-unavailable", "retirement diagnostic lost its project or cause");
        ++diagnostics;
        throw Error("storage-full", "diagnostic callback failure");
    };
    LocalProjectRequest request;
    request.home = home;
    const auto result = watch_local_projects(request, {root, sibling}, 1,
        [] { return false; }, {}, enrollment, [&](const fs::path& reported, const Json& state) {
            if (fenced || reported != root || state.at("phase") != "checking") return;
            StateStore binding_writer(workspace, true);
            set_local_project_lifecycle(workspace, binding, "detaching");
            fenced = true;
        }, [&](const LocalProjectRequest& request) {
            const auto directory = home / "projects" / digest(request.root.string());
            return Json{{"ok", true}, {"phase", "current"}, {"rebuilt", false},
                {"refresh_authority", "private-bootstrap"}, {"workspace_identity", directory_identity(directory, true)}};
        });
    check(fenced && removals == 1 && diagnostics == 1, "retirement failure boundary was not exercised");
    check(result.at("watch").at("root") == sibling.string() && result.at("watch").at("ticks") == 1 &&
          result.at("watch").at("phase") == "stopped", "retirement failure stopped or resurrected the wrong controller");
    StateStore retired_watch(workspace / "watch", false), sibling_watch(sibling_workspace / "watch", false);
    check(!retired_watch.writer_present() && !sibling_watch.writer_present(), "retirement leaked a controller writer");
    check(local_project_lifecycle(workspace) == LocalProjectLifecycle{"detaching", 1} &&
          require_local_project_active(sibling_workspace) == LocalProjectLifecycle{"active", 0},
          "retirement changed either project's lifecycle authority");
    check(StateStore(workspace, false).load() == std::optional<Json>(binding) &&
          StateStore(sibling_workspace, false).load() == std::optional<Json>(sibling_binding),
          "retirement changed preserved project bindings");
}

void transient_storage_failure_retries_without_restarting_controller() {
    for (const auto [failure, code] : {std::pair{ENOSPC, "storage-full"}, {EROFS, "storage-read-only"}, {EACCES, "access-denied"}}) {
        Fixture fixture;
        const auto root = fixture.root / "source", home = fixture.root / "home";
        fs::create_directory(root);
        ensure_private_directory(home);
        ensure_private_directory(home / "projects");
        const auto workspace = home / "projects" / digest(root.string());
        ensure_private_directory(workspace);
        { StateStore prepared_binding(workspace, true); }
        LocalProjectRequest request;
        request.root = root;
        request.home = home;
        bool injected = false, blocked = false, recovered = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        auto result = watch_local_projects(request, {root}, 0,
            [&] { return recovered || std::chrono::steady_clock::now() >= deadline; }, {}, {},
            [&](const fs::path&, const Json& state) {
                if (!injected && state.at("phase") == "checking") {
                    next_pending_error = failure;
                    injected = true;
                }
                if (state.at("phase") == "blocked") {
                    check(state.at("last_result").at("error").at("code") == code, "storage cause was lost");
                    check(state.at("state_persisted") == false, "failed write claimed persistence");
                    blocked = true;
                }
                if (blocked && state.at("phase") == "current") {
                    check(state.at("state_persisted") == true, "successful retry did not clear persistence failure");
                    recovered = true;
                }
            }, [&](const LocalProjectRequest&) {
                return Json{{"ok", true}, {"phase", "current"}, {"rebuilt", false},
                    {"refresh_authority", "private-bootstrap"}, {"workspace_identity", directory_identity(workspace, true)}};
            });
        check(injected && blocked && recovered, "storage failure did not recover automatically");
        check(result.at("watch").at("phase") == "stopped", "controller did not stop cleanly after recovery");
        check(next_pending_error == 0, "storage failure injection was not consumed");
        next_pending_error = failure;
        const auto bounded = watch_local_projects(request, {root}, 1,
            [&] { return std::chrono::steady_clock::now() >= deadline; }, {}, {}, {},
            [&](const LocalProjectRequest&) {
                return Json{{"ok", true}, {"phase", "current"}, {"rebuilt", false},
                    {"refresh_authority", "private-bootstrap"}, {"workspace_identity", directory_identity(workspace, true)}};
            });
        check(bounded.at("watch").at("ticks") == 1 && bounded.at("watch").at("failures") == 1,
            "a storage failure before the first sample escaped the finite sample limit");
    }
}
#endif
}

int main() {
    const struct {
        const char* name;
        void (*run)();
    } tests[] = {
        {"initial_stat_replacement_retries_boundedly", initial_stat_replacement_retries_boundedly},
        {"initial_stat_race_does_not_accept_unsafe_replacement", initial_stat_race_does_not_accept_unsafe_replacement},
        {"one_replacement_returns_new_json", one_replacement_returns_new_json},
        {"repeated_replacement_stops_after_three_attempts", repeated_replacement_stops_after_three_attempts},
        {"unsafe_replacement_is_not_bypassed", unsafe_replacement_is_not_bypassed},
        {"initial_unsafe_modes_are_not_retried", initial_unsafe_modes_are_not_retried},
#ifdef SHIN_TEST_LOCAL_CONTROLLER
        {"changing_watch_observation_does_not_replace_authority", changing_watch_observation_does_not_replace_authority},
        {"unsafe_watch_observation_is_not_ignored", unsafe_watch_observation_is_not_ignored},
        {"incomplete_lifecycle_intent_is_fenced_and_revisions_survive_reactivation", incomplete_lifecycle_intent_is_fenced_and_revisions_survive_reactivation},
        {"watch_admission_rejects_detach_and_reactivation_during_refresh", watch_admission_rejects_detach_and_reactivation_during_refresh},
        {"retirement_callback_failures_preserve_the_sibling_controller", retirement_callback_failures_preserve_the_sibling_controller},
        {"transient_storage_failure_retries_without_restarting_controller", transient_storage_failure_retries_without_restarting_controller},
#endif
    };
    unsigned failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
