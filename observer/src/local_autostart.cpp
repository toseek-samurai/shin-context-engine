#include "shin/observer/local_supervision.h"
#include "shin/observer/local_service.h"
#include "shin/observer/local_registry.h"
#include "shin/observer/observer.h"
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#ifdef __APPLE__
#include <fcntl.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif
#include <thread>
#ifdef __APPLE__
#include <unistd.h>
#endif

namespace shin::observer {
namespace {
#ifdef __APPLE__
using Clock = std::chrono::steady_clock;
void require(bool value, const char* code, const char* message) { if (!value) throw Error(code, message); }
bool entry_exists(const fs::path& path) { return fs::exists(fs::symlink_status(path)); }
fs::path canonical_home(const fs::path& path) {
    const auto home = fs::absolute(path).lexically_normal();
    (void)directory_identity(home, true);
    require(fs::canonical(home) == home, "unsafe-directory", "Service home must be canonical.");
    return home;
}
fs::path agent_directory() {
    std::array<char, 65536> buffer{}; passwd account{}; passwd* result = nullptr;
    require(::geteuid() != 0 && ::getpwuid_r(::geteuid(), &account, buffer.data(), buffer.size(), &result) == 0 &&
        result && account.pw_dir, "service-account-unavailable", "A non-root local user account is required.");
    return fs::canonical(account.pw_dir) / "Library/LaunchAgents";
}
void user_directory(const fs::path& path, bool create) {
    (void)directory_identity(path.parent_path());
    require(fs::canonical(path.parent_path()) == path.parent_path(), "unsafe-directory", "Agent parent must not use symlinks.");
    if (create && ::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
        throw Error("service-install-directory", "Cannot create the user agent directory.");
    (void)directory_identity(path);
    struct stat status{};
    require(::lstat(path.c_str(), &status) == 0 && status.st_uid == ::geteuid() && !(status.st_mode & 0022),
        "unsafe-directory", "Agent directory must be user-owned and not writable by others.");
}

int manager(const std::vector<std::string>& arguments) {
    const auto previous = std::signal(SIGCHLD, SIG_DFL);
    require(previous != SIG_ERR, "service-manager-failed", "Cannot establish manager child ownership.");
    struct Restore { decltype(previous) handler; ~Restore() { std::signal(SIGCHLD, handler); } } restore{previous};
    std::vector<std::string> words{"/bin/launchctl"}; words.insert(words.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv; argv.reserve(words.size() + 1);
    for (auto& word : words) argv.push_back(word.data());
    argv.push_back(nullptr);
    std::array<char*, 2> environment{const_cast<char*>("LC_ALL=C"), nullptr};
    posix_spawn_file_actions_t actions;
    require(::posix_spawn_file_actions_init(&actions) == 0, "service-manager-failed", "Cannot prepare manager process.");
    int error = 0;
    for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO})
        if (!error) error = ::posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == STDIN_FILENO ? O_RDONLY : O_WRONLY, 0);
    pid_t child = -1;
    if (!error) error = ::posix_spawn(&child, "/bin/launchctl", &actions, nullptr, argv.data(), environment.data());
    ::posix_spawn_file_actions_destroy(&actions);
    require(!error, "service-manager-failed", "Cannot start the OS service manager.");
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (Clock::now() < deadline) {
        int status = 0; const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        if (waited < 0 && errno != EINTR) throw Error("service-manager-failed", "Lost manager process ownership.");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(child, SIGKILL);
    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    throw Error("service-manager-timeout", "OS manager operation timed out; retry the explicit operation to reconcile its state.");
}
std::string domain() { return "gui/" + std::to_string(::geteuid()); }
std::string label(const fs::path& home) { return "org.shin.context." + digest(home.string()).substr(0, 32); }
bool loaded(const std::string& name) {
    const int code = manager({"print", domain() + "/" + name});
    require(code == 0 || code == 113, "service-manager-failed", "Cannot inspect the named user service.");
    return code == 0;
}
void checked(const std::vector<std::string>& args) {
    const int code = manager(args);
    if (code) throw Error("service-manager-failed", "OS manager rejected the operation (exit " + std::to_string(code) + "); installation state is preserved.");
}
std::string executable_hash(const fs::path& executable) {
    return digest(read_text(fs::canonical(executable), 128U * 1024U * 1024U, false));
}
std::optional<Json> receipt(const fs::path& home) {
    if (!entry_exists(home / "supervision")) return std::nullopt;
    StateStore state(home / "supervision", false);
    const auto record = state.load();
    if (!record) throw Error("service-install-incomplete", "Supervision state is incomplete; retry service-install.");
    const auto& value = *record;
    require(value.at("schema_version") == "shin.local-autostart.v1" && value.at("home_identity") == directory_identity(home, true) &&
        value.at("label") == label(home) && value.at("agent_path") == (agent_directory() / (label(home) + ".plist")).string(),
        "service-install-changed", "Installed service binding changed; no fallback is permitted.");
    const auto phase = value.at("phase").get<std::string>();
    require(phase == "installing" || phase == "installed" || phase == "updating" || phase == "removing" || phase == "removed",
        "service-install-changed", "Unknown installation phase.");
    const auto& definition = value.at("definition");
    require(definition.at("label") == label(home) && definition.at("service_home") == home.string() &&
        definition.at("unit").at("ProgramArguments") == Json::array({definition.at("unit").at("Program"), "_serve-supervised", "--home", home.string()}),
        "service-install-changed", "Installed service definition changed.");
    if (phase == "updating") {
        require(value.at("update_stage") == "stopping" || value.at("update_stage") == "starting",
            "service-install-changed", "Unknown update stage.");
        require(value.at("previous_plist").is_string() && value.at("previous_plist").get<std::string>().size() <= 32768 &&
            value.at("registry_sha256").is_string(), "service-install-changed", "Invalid update receipt.");
    }
    return record;
}
void verify_agent(const Json& value, bool allow_missing = false) {
    const fs::path path = value.at("agent_path").get<std::string>();
    user_directory(path.parent_path(), false);
    require(directory_identity(path.parent_path()) == value.at("agent_directory_identity").get<std::string>(), "service-install-changed", "Agent directory identity changed.");
    if (allow_missing && !entry_exists(path)) return;
    const auto content = read_text(path, 32768, true);
    require(content == value.at("definition").at("plist").get<std::string>() ||
        (value.at("phase") == "updating" && content == value.at("previous_plist").get<std::string>()),
        "service-install-changed", "Agent file changed; it must not be replaced or removed.");
}
void verify_binary(const Json& value, const fs::path& executable) {
    const auto binary = fs::canonical(executable);
    struct stat status{};
    require(::lstat(binary.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
        (status.st_uid == ::geteuid() || status.st_uid == 0) && !(status.st_mode & 06022) && ::access(binary.c_str(), X_OK) == 0,
        "unsafe-executable", "Installed executable permissions or ownership changed.");
    require(value.at("definition").at("unit").at("Program") == fs::canonical(executable).string() &&
        value.at("executable_sha256") == executable_hash(executable), "service-installed-kit-changed",
        "Installed kit changed; use the replacement executable's explicit service-update command.");
}
std::optional<Json> probe(const LocalProjectRequest& input, Clock::time_point deadline) {
    auto request = input; request.command = "service-status";
    try { return request_local_service(request, [&] { return Clock::now() >= deadline; }); }
    catch (const Error& error) {
        if (error.code != "service-unavailable" && error.code != "service-disconnected") throw;
        return std::nullopt;
    }
}
void wait_stopped(const fs::path& home) {
    const auto deadline = Clock::now() + std::chrono::seconds(55);
    while (Clock::now() < deadline) {
        if (!entry_exists(home / "service")) return;
        StateStore owner(home / "service", false);
        if (!owner.writer_present()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw Error("service-stop-timeout", "Service still owns its runtime lock; installation state is preserved.");
}
void unload_job(const Json& record) {
    const auto name = record.at("label").get<std::string>();
    if (!loaded(name)) return;
    checked({"bootout", domain() + "/" + name});
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (loaded(name)) {
        require(Clock::now() < deadline, "service-manager-timeout", "OS registration is still retiring; resume the explicit lifecycle operation.");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
Json start_job(const LocalProjectRequest& input, const Json& record) {
    verify_agent(record);
    const auto name = record.at("label").get<std::string>();
    const bool present = loaded(name);
    const auto deadline = Clock::now() + std::chrono::seconds(12);
    if (auto running = probe(input, deadline)) {
        require(present && running->value("supervision", "") == "launchd-user" && running->value("enrollment_mode", "") == "registry",
            "service-manager-conflict", "An unmanaged service is running in the installed home.");
        (*running)["started"] = false; return *running;
    }
    if (!present) checked({"bootstrap", domain(), record.at("agent_path")});
    else checked({"kickstart", domain() + "/" + name});
    while (Clock::now() < deadline) {
        if (auto running = probe(input, deadline)) {
            require(running->value("supervision", "") == "launchd-user" && running->value("enrollment_mode", "") == "registry",
                "service-manager-conflict", "Unexpected service answered managed startup.");
            (*running)["started"] = true; return *running;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    throw Error("service-manager-not-ready", "OS-managed service did not become ready; inspect the installation.");
}
Json result(const LocalProjectRequest& input, const Json& record) {
    return {{"schema_version", "shin.local-autostart-status.v1"}, {"tool", "shin-ce"}, {"version", SHIN_OBSERVER_VERSION},
        {"command", input.command}, {"ok", true}, {"phase", record.at("phase")}, {"manager", "launchd-user"},
        {"label", record.at("label")}, {"agent_path", record.at("agent_path")}, {"data_preserved", true}, {"readiness_assessed", false}};
}
#endif
}

std::optional<Json> start_installed_local_service(const LocalProjectRequest& input, const fs::path& executable) {
#ifdef __APPLE__
    const auto home = canonical_home(input.home);
    const auto record = receipt(home);
    if (!record) return std::nullopt;
    if (record->at("phase") == "removed") {
        require(!entry_exists(record->at("agent_path").get<std::string>()) && !loaded(record->at("label")),
            "service-manager-conflict", "Removed service registration unexpectedly exists.");
        return std::nullopt;
    }
    require(record->at("phase") == "installed", "service-install-incomplete", "Complete service-install/service-update/service-uninstall before starting the service.");
    verify_binary(*record, executable);
    return start_job(input, *record);
#else
    (void)executable;
    if (fs::exists(fs::symlink_status(fs::absolute(input.home) / "supervision")))
        throw Error("service-manager-unsupported", "An OS installation receipt exists but this platform cannot manage it.");
    return std::nullopt;
#endif
}

void check_local_service_supervision(const LocalProjectRequest& input, const fs::path& executable, bool supervised) {
#ifdef __APPLE__
    const auto record = receipt(canonical_home(input.home));
    if (!supervised) {
        require(!record || record->at("phase") == "removed", "service-managed-only", "This home requires its installed OS manager; use service-start.");
        return;
    }
    if (!record || (record->at("phase") != "installing" && record->at("phase") != "installed" &&
        !(record->at("phase") == "updating" && record->at("update_stage") == "starting")))
        throw Error("service-install-incomplete", "Managed service requires an active installation receipt.");
    verify_binary(*record, executable); verify_agent(*record);
    require(read_text(record->at("agent_path").get<std::string>(), 32768, true) == record->at("definition").at("plist").get<std::string>(),
        "service-install-incomplete", "The new service definition has not been published.");
#else
    (void)executable;
    if (supervised || fs::exists(fs::symlink_status(fs::absolute(input.home) / "supervision")))
        throw Error("service-manager-unsupported", "OS supervision is not implemented for this platform.");
#endif
}

Json local_autostart_status(const LocalProjectRequest& input, const fs::path& executable) {
#ifdef __APPLE__
    const auto record = receipt(canonical_home(input.home));
    if (!record) return {{"schema_version", "shin.local-autostart-status.v1"}, {"tool", "shin-ce"}, {"version", SHIN_OBSERVER_VERSION},
        {"ok", true}, {"phase", "not-installed"}, {"command", input.command}, {"manager", "launchd-user"}, {"readiness_assessed", false}};
    auto response = result(input, *record);
    response["loaded"] = loaded(record->at("label"));
    if (record->at("phase") == "updating") {
        response["update_stage"] = record->at("update_stage");
        response["next_action"] = "service-update";
        response["target_executable"] = record->at("definition").at("unit").at("Program");
        response["rollback_available"] = false;
    }
    if (record->at("phase") != "removed") {
        verify_agent(*record, record->at("phase") != "installed");
        response["kit_current"] = record->at("executable_sha256") == executable_hash(executable) &&
            record->at("definition").at("unit").at("Program") == fs::canonical(executable).string();
    }
    return response;
#else
    (void)input; (void)executable;
    throw Error("service-manager-unsupported", "OS installation is not implemented for this platform.");
#endif
}

Json configure_local_autostart(const LocalProjectRequest& input, const fs::path& executable) {
#ifdef __APPLE__
    require(input.command == "service-install" || input.command == "service-uninstall", "invalid-arguments", "Unsupported installation command.");
    const auto home = canonical_home(input.home);
    const auto lock = lock_local_service_control(home);
    checked({"print", domain()});
    const auto state_dir = home / "supervision";
    std::optional<Json> old;
    if (entry_exists(state_dir)) {
        StateStore existing(state_dir, false);
        if (existing.load()) old = receipt(home);
    }
    if (input.command == "service-uninstall") require(old.has_value(), "service-not-installed", "No recorded installation to remove.");
    require(!old || old->at("phase") != "updating", "service-update-incomplete", "Resume service-update with its recorded target executable first.");
    ensure_private_directory(state_dir);
    StateStore store(state_dir, true);
    if (input.command == "service-uninstall") {
        if (!old) throw Error("service-not-installed", "No recorded installation to remove.");
        auto record = *old;
        if (record.at("phase") == "removed") {
            require(!entry_exists(record.at("agent_path").get<std::string>()) && !loaded(record.at("label")),
                "service-manager-conflict", "Removed service registration unexpectedly exists; it will not be deleted.");
            return result(input, record);
        }
        verify_agent(record, true);
        record["phase"] = "removing"; store.save(record);
        unload_job(record);
        wait_stopped(home);
        verify_agent(record, true);
        const fs::path agent = record.at("agent_path").get<std::string>();
        if (entry_exists(agent)) remove_user_archive(agent, record.at("definition").at("plist"));
        record["phase"] = "removed"; store.save(record);
        return result(input, record);
    }
    require(input.command == "service-install", "invalid-arguments", "Unsupported installation command.");
    const auto definition = local_service_definition(input, executable, true);
    const auto agents = agent_directory();
    user_directory(agents.parent_path(), false); user_directory(agents, true);
    const auto agent = agents / (label(home) + ".plist");
    Json record;
    if (old && old->at("phase") != "removed") {
        record = *old;
        require(record.at("phase") != "removing", "service-install-incomplete", "Finish service-uninstall first.");
        verify_binary(record, executable); verify_agent(record, record.at("phase") == "installing");
    } else {
        require(!entry_exists(agent) && !loaded(label(home)), "service-install-conflict", "Unowned agent file or OS job exists; no takeover is permitted.");
        record = {{"schema_version", "shin.local-autostart.v1"}, {"phase", "installing"}, {"home_identity", directory_identity(home, true)},
            {"label", label(home)}, {"agent_path", agent.string()}, {"agent_directory_identity", directory_identity(agents)},
            {"definition", definition}, {"executable_sha256", executable_hash(executable)}};
        store.save(record);
    }
    if (record.at("phase") == "installing") {
        if (const auto running = probe(input, Clock::now() + std::chrono::seconds(10))) {
            require(running->value("enrollment_mode", "") == "registry", "service-mode-conflict", "Explicit-root service must be stopped separately.");
            if (running->value("supervision", "") != "launchd-user") {
                auto stop = input; stop.command = "service-stop"; (void)request_local_service(stop); wait_stopped(home);
            }
        }
        (void)write_user_archive(agent, record.at("definition").at("plist"));
    }
    const auto service = start_job(input, record);
    record["phase"] = "installed"; store.save(record);
    auto response = result(input, record);
    response["service_running"] = service.at("service_running"); response["instance_id"] = service.at("instance_id");
    return response;
#else
    (void)input; (void)executable;
    throw Error("service-manager-unsupported", "OS installation is not implemented for this platform.");
#endif
}

Json update_local_autostart(const LocalProjectRequest& input, const fs::path& executable) {
#ifdef __APPLE__
    const auto home = canonical_home(input.home);
    const auto lock = lock_local_service_control(home);
    checked({"print", domain()});
    const auto previous = receipt(home);
    if (!previous || (previous->at("phase") != "installed" && previous->at("phase") != "updating"))
        throw Error("service-not-installed", "An installed service is required; finish installation or removal first.");
    verify_agent(*previous);
    const auto definition = local_service_definition(input, executable, true);
    StateStore store(home / "supervision", true);
    store.recover_pending();
    require(store.load() == previous, "service-install-changed", "Installation state changed; retry the operation.");
    auto record = *previous;
    const auto registry_hash = [&] { return digest(read_text(home / "registry/state.json", kMaximumStateBytes, true)); };
    const auto registry_before = registry_hash();
    const auto projects = registered_local_projects(home);
    require(registry_hash() == registry_before, "registry-binding-changed", "Enrollment changed during update inventory; retry.");
    const auto hash = executable_hash(executable);
    const bool replaced_pending = record.at("phase") == "updating" && record.at("executable_sha256") != hash;
    if (replaced_pending) {
        // A later kit at the exact pending destination can finish an interrupted
        // update after full admission. A different route or OS unit is not adopted.
        require(record.at("definition").at("unit") == definition.at("unit"),
            "service-installed-kit-changed", "Pending update must retain its recorded executable and service definition.");
        require(registry_before == record.at("registry_sha256").get<std::string>(),
            "registry-binding-changed", "Enrollment changed during the pending update; preserve state for review.");
    }
    if (record.at("phase") == "installed" || replaced_pending) {
        if (record.at("executable_sha256") == hash && record.at("definition").at("unit").at("Program") == fs::canonical(executable).string()) {
            verify_binary(record, executable);
            const auto service = start_job(input, record);
            auto response = result(input, record);
            response.update({{"changed", false}, {"service_running", true}, {"instance_id", service.at("instance_id")}, {"rollback_available", false}});
            return response;
        }
        // Admission is read-only: do not migrate bindings, switch profiles or rebuild indexes.
        for (auto project : projects) {
            project.command = "status";
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            for (;;) {
                try {
                    const auto state = qualify_local_project(project, executable);
                    require(state.value("ok", false) && state.value("local_ready", false),
                        "service-update-project-not-ready", "A registered project is not ready under the replacement engine; the running service was not changed.");
                    break;
                } catch (const Error& error) {
                    if ((error.code != "project-busy" && error.code != "binding-changed") || Clock::now() >= deadline) throw;
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                }
            }
        }
        require(registry_hash() == registry_before, "registry-binding-changed", "Enrollment changed during update admission; retry.");
        verify_agent(record);
        record["previous_plist"] = read_text(record.at("agent_path").get<std::string>(), 32768, true);
        record["definition"] = definition;
        record["executable_sha256"] = hash;
        record["registry_sha256"] = registry_before;
        record["phase"] = "updating";
        record["update_stage"] = "stopping";
        store.save(record);
    }
    verify_binary(record, executable);
    require(registry_hash() == record.at("registry_sha256").get<std::string>(), "registry-binding-changed", "Enrollment changed during the pending update; preserve state for review.");
    if (record.at("update_stage") == "stopping") {
        unload_job(record);
        wait_stopped(home);
        verify_agent(record);
        replace_user_archive(record.at("agent_path").get<std::string>(), record.at("previous_plist"),
            record.at("definition").at("plist"), record.at("agent_directory_identity"));
        record["update_stage"] = "starting";
        store.save(record);
    }
    const auto service = start_job(input, record);
    for (auto project : projects) {
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        project.command = "status";
        for (;;) {
            try {
                auto health_request = project;
                health_request.command = "service-status";
                const auto health = request_local_service(health_request, [&] { return Clock::now() >= deadline; });
                const auto& admitted = health.at("projects");
                if (std::find(admitted.begin(), admitted.end(), Json(project.root.string())) != admitted.end()) {
                    const auto state = request_local_service(project, [&] { return Clock::now() >= deadline; });
                    if (state.value("ok", false) && state.value("local_ready", false)) break;
                }
            } catch (const Error& error) {
                if (error.code != "project-busy" && error.code != "binding-changed" &&
                    error.code != "service-project-not-ready" && error.code != "service-project-denied" &&
                    error.code != "project-updating") throw;
            }
            require(Clock::now() < deadline, "service-update-not-ready", "New service is not ready for every project; resume service-update after correcting the reported project condition.");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    require(registry_hash() == record.at("registry_sha256").get<std::string>(), "registry-binding-changed", "Enrollment changed before update completion.");
    verify_binary(record, executable); verify_agent(record);
    record["phase"] = "installed";
    for (const auto* key : {"previous_plist", "registry_sha256", "update_stage"}) record.erase(key);
    store.save(record);
    auto response = result(input, record);
    response.update({{"changed", true}, {"service_running", true}, {"instance_id", service.at("instance_id")},
        {"readiness_assessed", true}, {"ready_projects", projects.size()}, {"rollback_available", false},
        {"project_bindings_changed", false}, {"backup_created", false}});
    return response;
#else
    (void)input; (void)executable;
    throw Error("service-manager-unsupported", "OS-managed service update is not implemented for this platform.");
#endif
}
}
