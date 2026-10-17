#include "shin/observer/local_installation.h"
#include "shin/observer/local_service.h"
#include "shin/observer/local_supervision.h"
#include "shin/observer/local_registry.h"
#include <regex>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <stdio.h>
#endif

namespace shin::observer {
namespace {
constexpr auto marker_name = ".shin-installation.json";
constexpr auto manifest_name = "shin-kit.json";
constexpr std::size_t max_file = 128 * 1024 * 1024;
constexpr std::array commands{"shin-ce", "shin-context", "shin-observer", "shin-mcp", "shin-usage"};

void require(bool value, const char* code, const char* message) {
    if (!value) throw Error(code, message);
}
bool present(const fs::path& path) { return fs::exists(fs::symlink_status(path)); }
bool within(const fs::path& path, const fs::path& parent) {
    auto child = path.begin();
    for (auto part = parent.begin(); part != parent.end(); ++part, ++child)
        if (child == path.end() || *child != *part) return false;
    return true;
}
fs::path normalized(const fs::path& path) { return fs::absolute(path).lexically_normal(); }
void user_directory(const fs::path& path, bool create) {
    if (!present(path) && create) {
        user_directory(path.parent_path(), true);
        ensure_private_directory(path);
    }
    (void)directory_identity(path);
    struct stat status{};
    require(::lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
        status.st_uid == ::geteuid() && !(status.st_mode & 0022) && fs::canonical(path) == path,
        "installation-path-unsafe", "Installation directories must be user-owned, non-symlink paths, not writable by others.");
}
fs::path control_path(const fs::path& prefix) {
    return prefix.parent_path() / (".shin-install-" + digest(prefix.string()).substr(0, 24));
}
fs::path executable_prefix(const fs::path& executable) {
    const auto file = fs::canonical(executable);
    if (file.parent_path().filename() == "MacOS" && file.parent_path().parent_path().filename() == "Contents")
        return file.parent_path().parent_path().parent_path().parent_path();
    return file.parent_path().parent_path();
}
Json validate_manifest(Json value) {
    require(value.is_object() && value.size() == 4 && value.value("schema_version", "") == "shin.local-kit.v1" &&
        value.contains("files") && value.at("files").is_array() && !value.at("files").empty() && value.at("files").size() <= 1024 &&
        value.contains("version") && value.at("version").is_string() &&
        value.contains("core_version") && value.at("core_version").is_string(),
        "installation-manifest-invalid", "A complete native kit manifest is required.");
    std::set<std::string> paths;
    std::uint64_t total = 0;
    for (const auto& file : value.at("files")) {
        require(file.is_object() && file.size() == 4 && file.contains("path") && file.at("path").is_string() &&
            file.contains("sha256") && file.at("sha256").is_string() && file.contains("size") && file.at("size").is_number_unsigned() &&
            file.contains("executable") && file.at("executable").is_boolean(),
            "installation-manifest-invalid", "Malformed kit file entry.");
        const auto name = file.at("path").get<std::string>();
        const fs::path path(name);
        const auto hash = file.at("sha256").get<std::string>();
        require(!name.empty() && name.size() <= 1024 && name.find_first_of("\\\r\n") == std::string::npos &&
            name.find('\0') == std::string::npos && !path.is_absolute() && path.lexically_normal() == path &&
            path.filename() != "." && name != marker_name && name != manifest_name &&
            std::none_of(path.begin(), path.end(), [](const auto& part) { return part == ".." || part.empty(); }) &&
            paths.insert(name).second && hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos &&
            file.at("size").get<std::uint64_t>() <= max_file,
            "installation-manifest-invalid", "Unsafe, duplicate or oversized kit file entry.");
        total += file.at("size").get<std::uint64_t>();
    }
    require(total <= 512 * 1024 * 1024, "installation-manifest-invalid", "Kit exceeds its size limit.");
    for (const auto* command : commands)
        require(paths.contains(std::string("bin/") + command), "installation-manifest-invalid", "The kit is missing a required executable.");
    require(paths.contains("share/shin-context/profiles/generic-source.json") &&
        paths.contains("Shin Monitor.app/Contents/MacOS/Shin Monitor"),
        "installation-manifest-invalid", "The kit is missing its profiles or Monitor.");
    return value;
}
Json manifest(const fs::path& prefix) {
    return validate_manifest(parse_json(read_text(prefix / manifest_name, kMaximumStateBytes / 2, false)));
}
std::set<fs::path> directories(const Json& kit) {
    std::set<fs::path> result;
    for (const auto& file : kit.at("files"))
        for (auto path = fs::path(file.at("path").get<std::string>()).parent_path(); !path.empty(); path = path.parent_path())
            result.insert(path);
    return result;
}
void regular_file(const fs::path& path) {
    struct stat status{};
    require(::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) && status.st_nlink == 1 &&
        status.st_uid == ::geteuid() && !(status.st_mode & 07022),
        "installation-file-unsafe", "Kit files must be singly linked, user-owned regular files without special or shared-write permissions.");
}
void inventory(const fs::path& prefix, const Json& kit, bool installed) {
    std::set<fs::path> files{manifest_name};
    if (installed) files.insert(marker_name);
    for (const auto& file : kit.at("files")) files.insert(file.at("path").get<std::string>());
    const auto dirs = directories(kit);
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(prefix)) {
        require(++count <= files.size() + dirs.size(), "installation-extra-file", "The kit contains unowned entries.");
        const auto path = entry.path().lexically_relative(prefix);
        const auto type = entry.symlink_status().type();
        if (type == fs::file_type::directory) {
            require(dirs.contains(path), "installation-extra-file", "The kit contains an unowned directory.");
            user_directory(entry.path(), false);
        } else {
            require(type == fs::file_type::regular && files.contains(path), "installation-extra-file", "The kit contains an unowned file or link.");
            regular_file(entry.path());
        }
    }
}
void verify_files(const fs::path& prefix, const Json& kit, bool installed, bool allow_missing = false) {
    inventory(prefix, kit, installed);
    for (const auto& file : kit.at("files")) {
        const auto path = prefix / file.at("path").get<std::string>();
        if (allow_missing && !present(path)) continue;
        regular_file(path);
        require(!file.at("executable").get<bool>() || (::access(path.c_str(), X_OK) == 0),
            "installation-file-changed", "A kit executable is no longer executable.");
        const auto bytes = read_text(path, max_file, false);
        require(bytes.size() == file.at("size") && file.at("sha256") == digest(bytes),
            "installation-file-changed", "Kit content differs from its manifest; no installed files were replaced.");
    }
}
void verify_record_locations(const fs::path& prefix, const Json& value) {
    require(value.is_object() && value.size() == 10 && value.value("schema_version", "") == "shin.local-installation.v1" &&
        value.value("prefix", "") == prefix.string() && value.value("control", "") == control_path(prefix).string() &&
        value.value("control_identity", "") == directory_identity(control_path(prefix), true),
        "installation-ownership-changed", "Installation ownership or location changed.");
    const fs::path home = value.at("home").get<std::string>();
    const fs::path bin = value.at("bin_dir").get<std::string>();
    require(home == normalized(home) && bin == normalized(bin) &&
        value.at("home_identity") == directory_identity(home, true) &&
        value.at("bin_dir_identity") == directory_identity(bin) &&
        !within(home, prefix) && !within(prefix, home) && !within(bin, prefix) && !within(prefix, bin),
        "installation-ownership-changed", "Installation data or command directory changed.");
}
Json record(const fs::path& prefix) {
    const auto value = parse_json(read_text(prefix / marker_name, kMaximumStateBytes, true));
    verify_record_locations(prefix, value);
    require(value.at("prefix_identity") == directory_identity(prefix, true),
        "installation-ownership-changed", "Installed payload directory changed.");
    return value;
}
Json state_record(const StateStore& store, const Json& receipt, bool active, bool startup = false) {
    const auto state = store.load();
    if (!state || state->value("schema_version", "") != "shin.local-install-operation.v1" ||
        state->at("receipt") != receipt || (active && state->at("phase") != "installed" && !(startup && state->at("phase") == "activating")))
        throw Error("installation-incomplete", "Installation is incomplete; repeat the original install or uninstall command from the extracted kit.");
    return *state;
}
void links(const Json& receipt, bool create, bool remove, bool allow_missing = false) {
    const fs::path bin = receipt.at("bin_dir").get<std::string>();
    const fs::path prefix = receipt.at("prefix").get<std::string>();
    require(receipt.at("bin_dir_identity") == directory_identity(bin), "installation-ownership-changed", "Command directory changed.");
    for (const auto* name : commands) {
        const auto path = bin / name;
        const auto target = prefix / "bin" / name;
        if (present(path)) {
            require(fs::is_symlink(fs::symlink_status(path)) && fs::read_symlink(path) == target,
                "installation-command-conflict", "An existing command is not owned by this installation; it was preserved.");
            if (remove) fs::remove(path);
        } else if (create) {
            fs::create_symlink(target, path);
        } else if (!remove && !allow_missing) throw Error("installation-command-missing", "Installed command link is missing; repeat install from the same kit.");
    }
}
void write_payload(const fs::path& path, const std::string& bytes, bool executable) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, executable ? 0755 : 0644);
    require(fd >= 0, "installation-copy-failed", "Cannot create an exclusive kit file.");
    struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
    require(::fchmod(fd, executable ? 0755 : 0644) == 0, "installation-copy-failed", "Cannot set kit file permissions.");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto size = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (size < 0 && errno == EINTR) continue;
        require(size > 0, "installation-copy-failed", "Cannot write a kit file; installation can be retried.");
        offset += static_cast<std::size_t>(size);
    }
    require(::fsync(fd) == 0, "installation-copy-failed", "Cannot synchronize a kit file.");
}
void sync_directory(const fs::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    require(fd >= 0, "installation-sync-failed", "Cannot open the installation directory.");
    const int result = ::fsync(fd);
    ::close(fd);
    require(result == 0, "installation-sync-failed", "Cannot synchronize the installation directory.");
}
void remove_payload(const fs::path& prefix, const Json& kit, const std::string& identity) {
    require(directory_identity(prefix, true) == identity, "installation-ownership-changed", "Payload directory changed.");
    inventory(prefix, kit, true);
    // No recursive deletion: only the manifest's names and empty owned directories.
    for (const auto& file : kit.at("files")) fs::remove(prefix / file.at("path").get<std::string>());
    fs::remove(prefix / manifest_name);
    fs::remove(prefix / marker_name);
    const auto dirs = directories(kit);
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) fs::remove(prefix / *it);
    require(fs::remove(prefix), "installation-remove-failed", "The owned installation directory is not empty.");
    sync_directory(prefix.parent_path());
}
std::unique_ptr<StateStore> stop_owned_service(const Json& receipt, const std::string& observer_version = SHIN_OBSERVER_VERSION) {
    LocalProjectRequest request;
    request.home = receipt.at("home").get<std::string>();
    const auto executable = fs::path(receipt.at("prefix").get<std::string>()) / "bin/shin-ce";
    if (present(request.home / "supervision")) {
        request.command = "service-installation";
        const auto status = local_autostart_status(request, executable);
        if (status.at("phase") != "not-installed" && status.at("phase") != "removed") {
            require(status.value("kit_current", false), "installation-service-conflict", "The OS service belongs to another kit; it was preserved.");
            request.command = "service-uninstall";
            (void)configure_local_autostart(request, executable);
        }
    }
    auto control = lock_local_service_control(request.home);
    if (!present(request.home / "service")) return control;
    StateStore service(request.home / "service", false);
    if (service.writer_present()) {
        request.command = "service-stop";
        (void)control_local_service_for_update(request.home, observer_version, true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(55);
        while (service.writer_present()) {
            require(std::chrono::steady_clock::now() < deadline, "installation-service-busy", "Service is still stopping; repeat uninstall.");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    return control;
}
// Update journals retain both complete payload identities. Data and project
// bindings are never copied, rewritten or included in rollback snapshots.
std::array<unsigned long, 5> release_order(const std::string& version) {
    static const std::regex pattern(R"(^([0-9]{1,9})\.([0-9]{1,9})\.([0-9]{1,9})(?:-(dev|rc)\.([0-9]{1,9}))?$)");
    std::smatch match;
    require(std::regex_match(version, match, pattern), "installation-version-invalid", "Kit version is not a supported release identity.");
    return {std::stoul(match[1]), std::stoul(match[2]), std::stoul(match[3]),
        match[4] == "dev" ? 0UL : match[4] == "rc" ? 1UL : 2UL, match[5].matched ? std::stoul(match[5]) : 0UL};
}
void qualify_update(const fs::path& home, const fs::path& executable) {
    if (!present(home / "registry")) return;
    for (auto project : registered_local_projects(home)) {
        project.command = "status";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            try {
                const auto result = qualify_local_project(project, executable);
                require(result.value("ok", false) && result.value("local_ready", false),
                    "installation-project-not-ready", "A project is not ready under the new engine; the current kit was preserved.");
                break;
            } catch (const Error& error) {
                if ((error.code != "project-busy" && error.code != "binding-changed") ||
                    std::chrono::steady_clock::now() >= deadline) throw;
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
        }
    }
}
std::string registry_digest(const fs::path& home) {
    return present(home / "registry/state.json") ? digest(read_text(home / "registry/state.json", kMaximumStateBytes, true)) : "";
}
Json runtime_before_update(const fs::path& home, const fs::path& executable) {
    Json runtime{{"managed", false}, {"running", false}};
    LocalProjectRequest request; request.home = home; request.command = "service-installation";
    if (present(home / "supervision")) {
        const auto status = local_autostart_status(request, executable);
        if (status.at("phase") != "not-installed" && status.at("phase") != "removed") {
            require(status.at("phase") == "installed" && status.value("kit_current", false),
                "installation-service-conflict", "Finish the existing service operation using its owning kit first.");
            runtime["managed"] = true;
        }
    }
    if (present(home / "service")) {
        StateStore service(home / "service", false);
        if (service.writer_present()) {
            request.command = "service-status";
            const auto status = control_local_service_for_update(home, manifest(executable.parent_path().parent_path()).at("version"), false);
            require(status.value("enrollment_mode", "") == "registry", "installation-service-conflict", "Explicit-root services must be stopped before updating this kit.");
            runtime["running"] = true;
        }
    }
    return runtime;
}
void wait_update_service_stopped(const fs::path& home) {
    StateStore service(home / "service", false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(55);
    while (service.writer_present()) {
        require(std::chrono::steady_clock::now() < deadline, "installation-service-busy", "Service is still stopping; repeat update.");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
void resume_update_runtime(const fs::path& home, const fs::path& executable, const Json& runtime) {
    LocalProjectRequest request; request.home = home;
    if (runtime.at("managed")) {
        request.command = "service-install";
        (void)configure_local_autostart(request, executable);
    }
    if (runtime.at("running") || runtime.at("managed")) {
        request.command = "service-start";
        const auto status = start_local_service(request, executable);
        require(status.value("registry_error", std::string{}).empty() && status.at("enrollment_errors").empty(),
            "installation-service-not-ready", "Updated service could not admit its registered projects.");
        qualify_update(home, executable);
        // A registered but deliberately stopped service stays stopped.
        if (!runtime.at("running").get<bool>()) {
            request.command = "service-stop";
            (void)request_local_service(request);
            wait_update_service_stopped(home);
        }
    }
}
// Rollback must use the verified previous CLI's protocol and qualification rules.
// No shell, inherited user environment, or arbitrary subcommand is involved.
Json previous_cli(const fs::path& executable, const fs::path& home,
                  const char* command, const fs::path& root = {}) {
    const auto previous = std::signal(SIGCHLD, SIG_DFL);
    require(previous != SIG_ERR, "installation-recovery-process", "Cannot establish recovery child ownership.");
    struct Restore { decltype(previous) handler; ~Restore() { std::signal(SIGCHLD, handler); } } restore{previous};
    int channel[2];
    require(::pipe(channel) == 0, "installation-recovery-process", "Cannot create recovery output channel.");
    struct Channel { int* fds; ~Channel() { for (int i = 0; i < 2; ++i) if (fds[i] >= 0) ::close(fds[i]); } } cleanup{channel};
    require(::fcntl(channel[0], F_SETFD, FD_CLOEXEC) == 0 && ::fcntl(channel[1], F_SETFD, FD_CLOEXEC) == 0 &&
        ::fcntl(channel[0], F_SETFL, O_NONBLOCK) == 0,
        "installation-recovery-process", "Cannot protect recovery output channel.");
    std::vector<std::string> words{executable.string(), command, "--home", home.string()};
    if (!root.empty()) { words.emplace_back("--root"); words.push_back(root.string()); }
    std::vector<char*> argv; argv.reserve(words.size() + 1);
    for (auto& word : words) argv.push_back(word.data());
    argv.push_back(nullptr);
    std::array<char*, 2> environment{const_cast<char*>("LC_ALL=C"), nullptr};
    posix_spawn_file_actions_t actions;
    require(::posix_spawn_file_actions_init(&actions) == 0, "installation-recovery-process", "Cannot prepare recovery process.");
    int error = ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    for (int fd : {STDOUT_FILENO, STDERR_FILENO})
        if (!error) error = ::posix_spawn_file_actions_adddup2(&actions, channel[1], fd);
    pid_t child = -1;
    if (!error) error = ::posix_spawn(&child, executable.c_str(), &actions, nullptr, argv.data(), environment.data());
    ::posix_spawn_file_actions_destroy(&actions);
    require(!error, "installation-recovery-process", "Cannot start the verified previous CLI.");
    struct Child {
        pid_t pid;
        ~Child() { if (pid > 0) { ::kill(pid, SIGKILL); while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {} } }
    } reap{child};
    ::close(channel[1]); channel[1] = -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    std::string output;
    int status = 0;
    bool ended = false, eof = false;
    while (!ended || !eof) {
        require(std::chrono::steady_clock::now() < deadline, "installation-recovery-timeout", "Previous CLI did not complete; repeat update to resume recovery.");
        std::array<char, 8192> bytes{};
        const auto count = ::read(channel[0], bytes.data(), bytes.size());
        if (count > 0) {
            require(output.size() + static_cast<std::size_t>(count) <= kMaximumStateBytes,
                "installation-recovery-output", "Previous CLI exceeded the bounded recovery response.");
            output.append(bytes.data(), static_cast<std::size_t>(count));
        } else if (count == 0) eof = true;
        else require(errno == EAGAIN || errno == EINTR, "installation-recovery-output", "Cannot read recovery response.");
        if (!ended) {
            const auto waited = ::waitpid(child, &status, WNOHANG);
            require(waited >= 0 || errno == EINTR, "installation-recovery-process", "Lost recovery child ownership.");
            if (waited == child) { ended = true; reap.pid = -1; }
        }
        if (count <= 0 && (!ended || !eof)) {
            if (eof) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            else { pollfd wait{channel[0], POLLIN, 0}; (void)::poll(&wait, 1, 10); }
        }
    }
    require(WIFEXITED(status), "installation-recovery-process", "Previous CLI terminated unexpectedly.");
    const auto result = parse_json(output);
    if (WEXITSTATUS(status) != 0) {
        const auto code = result.contains("error") ? validation_identifier(result.at("error").value("code", "")) : "";
        throw Error("installation-recovery-command", "Previous CLI could not restore the service (" + code + ").");
    }
    require(result.is_object() && result.value("ok", false), "installation-recovery-output", "Previous CLI did not confirm recovery.");
    return result;
}
void resume_previous_runtime(const fs::path& home, const fs::path& executable, const Json& runtime) {
    if (runtime.at("managed")) (void)previous_cli(executable, home, "service-install");
    if (!runtime.at("running").get<bool>() && !runtime.at("managed").get<bool>()) return;
    const auto status = previous_cli(executable, home, "service-start");
    require(status.value("registry_error", std::string{}).empty() && status.at("enrollment_errors").empty(),
        "installation-service-not-ready", "Previous service could not admit its registered projects.");
    if (present(home / "registry")) for (const auto& project : registered_local_projects(home)) {
        const auto result = previous_cli(executable, home, "status", project.root);
        require(result.value("local_ready", false), "installation-project-not-ready", "A project is not ready after rollback.");
    }
    if (!runtime.at("running").get<bool>()) {
        (void)previous_cli(executable, home, "service-stop");
        wait_update_service_stopped(home);
    }
}
void verify_update_payload(const fs::path& path, const Json& kit, const Json& receipt) {
    require(directory_identity(path, true) == receipt.at("prefix_identity").get<std::string>() &&
        digest(read_text(path / manifest_name, kMaximumStateBytes, false)) == receipt.at("manifest_sha256").get<std::string>() &&
        manifest(path) == kit && parse_json(read_text(path / marker_name, kMaximumStateBytes, true)) == receipt,
        "installation-ownership-changed", "An update payload, manifest or receipt changed.");
    verify_files(path, kit, true);
}
void clear_previous(Json& state, StateStore& store, const fs::path& stage) {
    if (!state.contains("previous")) {
        require(!present(stage), "installation-not-owned", "An unowned update payload was preserved.");
        return;
    }
    const auto previous = state.at("previous");
    if (present(stage)) {
        verify_update_payload(stage, validate_manifest(previous.at("kit")), previous.at("receipt"));
        remove_payload(stage, previous.at("kit"), previous.at("receipt").at("prefix_identity").get<std::string>());
    }
    state.erase("previous"); store.save(state);
}
void copy_update_payload(const fs::path& source, const fs::path& stage, Json& state, StateStore& store) {
    auto& update = state["update"];
    const auto kit = validate_manifest(update.at("kit"));
    auto& receipt = update["receipt"];
    if (present(stage)) {
        if (receipt.at("prefix_identity").get<std::string>() == "") {
            (void)directory_identity(stage, true);
            require(fs::is_empty(stage), "installation-not-owned", "An uncommitted update directory is not empty.");
            fs::remove(stage);
        } else remove_payload(stage, kit, receipt.at("prefix_identity").get<std::string>());
    }
    ensure_private_directory(stage, true);
    receipt["prefix_identity"] = directory_identity(stage, true); store.save(state);
    for (const auto& path : directories(kit)) ensure_private_directory(stage / path);
    for (const auto& file : kit.at("files")) {
        const auto name = file.at("path").get<std::string>();
        const auto bytes = read_text(source / name, max_file, false);
        require(bytes.size() == file.at("size") && digest(bytes) == file.at("sha256").get<std::string>(),
            "installation-file-changed", "New kit changed while copying; the current kit was preserved.");
        write_payload(stage / name, bytes, file.at("executable"));
    }
    const auto bytes = read_text(source / manifest_name, kMaximumStateBytes, false);
    require(digest(bytes) == receipt.at("manifest_sha256").get<std::string>(), "installation-file-changed", "New kit manifest changed during copying.");
    write_payload(stage / manifest_name, bytes, false);
    (void)write_private_archive(stage / marker_name, receipt.dump());
    verify_files(stage, kit, true);
    for (const auto& path : directories(kit)) sync_directory(stage / path);
    sync_directory(stage); sync_directory(stage.parent_path());
    update["step"] = "stopping"; store.save(state);
}
void swap_update_payload(const fs::path& prefix, const fs::path& stage,
                         const Json& before, const Json& after) {
    const auto current = directory_identity(prefix, true);
    if (current == after.at("prefix_identity").get<std::string>()) {
        require(directory_identity(stage, true) == before.at("prefix_identity").get<std::string>(),
            "installation-ownership-changed", "Previous payload identity changed after switching.");
        return; // The atomic switch committed before its journal acknowledgement.
    }
    require(current == before.at("prefix_identity").get<std::string>() && directory_identity(stage, true) == after.at("prefix_identity").get<std::string>(),
        "installation-ownership-changed", "A payload directory changed before switching.");
#ifdef __APPLE__
    require(::renamex_np(prefix.c_str(), stage.c_str(), RENAME_SWAP) == 0,
        "installation-publish-failed", "Cannot atomically exchange the installed and staged kits.");
#else
    throw Error("installation-platform-unsupported", "Atomic kit updates are qualified only on macOS.");
#endif
    sync_directory(prefix.parent_path()); sync_directory(stage.parent_path());
}
Json adopt_update_prefix(const fs::path& prefix, const fs::path& bin, const fs::path& home,
                         StateStore& store, std::optional<Json> saved) {
    const auto control = control_path(prefix);
    Json state;
    if (saved) {
        state = *saved;
        require(state.at("phase") == "adopting", "installation-incomplete", "Complete the existing installation operation first.");
        verify_files(prefix, state.at("kit"), present(prefix / marker_name));
        require(state.at("receipt").at("prefix_identity").get<std::string>() == directory_identity(prefix),
            "installation-ownership-changed", "Legacy kit changed during adoption.");
    } else {
        require(!present(prefix / marker_name), "installation-ownership-changed", "An existing receipt must not be adopted as an unowned kit.");
        const auto kit = manifest(prefix); verify_files(prefix, kit, false);
        require(present(home / "registry/state.json") && present(home / "identity/state.json"),
            "installation-home-conflict", "Explicit adoption requires an existing Shin registry and identity.");
        (void)registered_local_projects(home);
        user_directory(prefix, false); user_directory(bin, false); (void)directory_identity(home, true);
        const auto receipt = Json{{"schema_version", "shin.local-installation.v1"}, {"prefix", prefix.string()},
            {"prefix_identity", directory_identity(prefix)}, {"control", control.string()},
            {"control_identity", directory_identity(control, true)}, {"home", home.string()},
            {"home_identity", directory_identity(home, true)}, {"bin_dir", bin.string()},
            {"bin_dir_identity", directory_identity(bin)},
            {"manifest_sha256", digest(read_text(prefix / manifest_name, kMaximumStateBytes, false))}};
        links(receipt, false, false); // Adoption does not claim or replace unrelated commands.
        (void)runtime_before_update(home, prefix / "bin/shin-ce");
        if (present(home / "installation")) {
            StateStore claim(home / "installation", false);
            require(claim.load() == Json{{"schema_version", "shin.local-install-home.v1"}, {"prefix", prefix.string()},
                {"control_identity", directory_identity(control, true)}}, "installation-home-conflict", "Existing data belongs to another installer.");
        }
        state = {{"schema_version", "shin.local-install-operation.v1"}, {"phase", "adopting"}, {"receipt", receipt}, {"kit", kit}};
        store.save(state);
    }
    // Only explicitly adopted, fully inventoried payload permissions are tightened.
    const int directory = ::open(prefix.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    require(directory >= 0, "installation-path-unsafe", "Cannot open the adopted payload safely.");
    const int protected_mode = ::fchmod(directory, 0700);
    ::close(directory);
    require(protected_mode == 0, "installation-path-unsafe", "Cannot protect the adopted payload.");
    ensure_private_directory(home / "installation");
    StateStore claim(home / "installation", true); claim.recover_pending();
    const auto expected = Json{{"schema_version", "shin.local-install-home.v1"}, {"prefix", prefix.string()},
        {"control_identity", directory_identity(control, true)}};
    if (const auto old = claim.load()) require(*old == expected, "installation-home-conflict", "Data-home ownership changed during adoption.");
    else claim.save(expected);
    (void)write_private_archive(prefix / marker_name, state.at("receipt").dump());
    require(record(prefix) == state.at("receipt"), "installation-ownership-changed", "Adopted receipt changed.");
    state["phase"] = "installed"; store.save(state);
    return state;
}
Json update_kit(const fs::path& source, const fs::path& prefix, const fs::path& bin, const fs::path& home,
                bool adopt, std::unique_ptr<StateStore>& owner, std::optional<Json> saved) {
    const auto control = control_path(prefix), stage = control / "update-payload";
    const auto kit = manifest(source);
    require(kit.at("version") == SHIN_OBSERVER_VERSION && kit.at("core_version") == SHIN_VERSION,
        "installation-version-mismatch", "Run update from the same complete new kit.");
    verify_files(source, kit, present(source / marker_name));
    const auto hash = digest(read_text(source / manifest_name, kMaximumStateBytes, false));
    if (!saved || saved->at("phase") == "adopting") {
        require(adopt, "installation-not-owned", "Use explicit --adopt-existing with the verified legacy prefix, home and command directory.");
        qualify_update(home, source / "bin/shin-ce");
        saved = adopt_update_prefix(prefix, bin, home, *owner, saved);
    }
    auto state = *saved;
    require(state.at("phase") == "installed" || state.at("phase") == "updating" ||
        state.at("phase") == "activating" || state.at("phase") == "rolling-back",
        "installation-incomplete", "Complete the pending installation operation before updating.");
    verify_record_locations(prefix, state.at("receipt"));
    if (state.contains("resume_runtime")) {
        verify_update_payload(prefix, state.at("kit"), state.at("receipt"));
        owner.reset();
        resume_previous_runtime(home, prefix / "bin/shin-ce", state.at("resume_runtime"));
        owner = std::make_unique<StateStore>(control, true);
        require(owner->load() == state, "installation-ownership-changed", "Recovery state changed during restart.");
        state.erase("resume_runtime"); owner->save(state);
        throw Error("installation-update-rolled-back", "Interrupted rollback completed; the previous kit and service are restored.");
    }
    if (state.at("phase") == "installed" && !state.contains("update")) {
        require(record(prefix) == state.at("receipt"), "installation-ownership-changed", "Installed receipt changed.");
        verify_update_payload(prefix, state.at("kit"), state.at("receipt")); links(state.at("receipt"), false, false);
        if (state.at("receipt").at("manifest_sha256").get<std::string>() == hash)
            return {{"ok", true}, {"command", "update"}, {"changed", false}, {"phase", "installed"}, {"version", kit.at("version")}, {"data_preserved", true}};
        require(kit.at("core_version") == state.at("kit").at("core_version"),
            "installation-core-transition-unsupported", "This updater preserves the current Core data contract; a different Core version requires an explicit migration.");
        require(release_order(kit.at("version")) > release_order(state.at("kit").at("version")),
            "installation-version-not-newer", "Update requires a newer kit version; altered same-version kits and downgrades are refused.");
        const auto registry = registry_digest(home);
        qualify_update(home, source / "bin/shin-ce");
        const auto runtime = runtime_before_update(home, prefix / "bin/shin-ce");
        require(registry_digest(home) == registry, "registry-binding-changed", "Project enrollment changed during update admission.");
        clear_previous(state, *owner, stage);
        auto next = state.at("receipt"); next["prefix_identity"] = ""; next["manifest_sha256"] = hash;
        state["update"] = {{"receipt", next}, {"kit", kit}, {"old_receipt", state.at("receipt")},
            {"old_kit", state.at("kit")}, {"runtime", runtime}, {"registry_sha256", registry}, {"step", "copying"}};
        state["phase"] = "updating"; owner->save(state);
    }
    require(state.contains("update") && state.at("update").at("receipt").at("manifest_sha256").get<std::string>() == hash,
        "installation-options-conflict", "Resume the update from its original extracted kit.");
    auto update = state.at("update");
    require(validate_manifest(update.at("kit")) == kit, "installation-state-invalid", "Pending target inventory changed.");
    (void)validate_manifest(update.at("old_kit"));
    for (const auto* field : {"receipt", "old_receipt"}) {
        verify_record_locations(prefix, update.at(field));
        require(update.at(field).at("home") == home.string() && update.at(field).at("bin_dir") == bin.string(),
            "installation-state-invalid", "Pending update locations changed.");
    }
    require(registry_digest(home) == update.at("registry_sha256").get<std::string>(), "registry-binding-changed", "Enrollment changed during a pending update; files were preserved for review.");
    if (state.at("phase") == "updating" && update.at("step") == "copying") {
        copy_update_payload(source, stage, state, *owner); update = state.at("update");
    }
    const auto rollback = [&] {
        if (!owner) owner = std::make_unique<StateStore>(control, true);
        state["phase"] = "rolling-back"; owner->save(state);
        const auto current = directory_identity(prefix, true);
        const auto current_receipt = current == update.at("receipt").at("prefix_identity").get<std::string>() ? update.at("receipt") : update.at("old_receipt");
        auto service_guard = stop_owned_service(current_receipt, current == update.at("receipt").at("prefix_identity").get<std::string>() ? update.at("kit").at("version").get<std::string>() : update.at("old_kit").at("version").get<std::string>());
        if (current == update.at("receipt").at("prefix_identity").get<std::string>()) {
            verify_update_payload(stage, update.at("old_kit"), update.at("old_receipt"));
            swap_update_payload(prefix, stage, update.at("receipt"), update.at("old_receipt"));
        } else require(current == update.at("old_receipt").at("prefix_identity").get<std::string>(), "installation-ownership-changed", "Rollback destination changed.");
        state["receipt"] = update.at("old_receipt"); state["kit"] = update.at("old_kit");
        verify_update_payload(prefix, state.at("kit"), state.at("receipt"));
        state["phase"] = "installed"; state.erase("update");
        state["resume_runtime"] = update.at("runtime");
        state["previous"] = {{"receipt", update.at("receipt")}, {"kit", update.at("kit")}};
        owner->save(state); service_guard.reset(); owner.reset();
        resume_previous_runtime(home, prefix / "bin/shin-ce", update.at("runtime"));
        owner = std::make_unique<StateStore>(control, true);
        require(owner->load() == state, "installation-ownership-changed", "Rollback state changed during restart.");
        state.erase("resume_runtime"); owner->save(state);
    };
    if (state.at("phase") == "rolling-back") {
        rollback();
        throw Error("installation-update-rolled-back", "Interrupted rollback completed; the previous kit is active and project data was preserved.");
    }
    try {
        if (state.at("phase") == "updating") {
            if (directory_identity(prefix, true) == update.at("old_receipt").at("prefix_identity").get<std::string>()) {
                verify_update_payload(prefix, update.at("old_kit"), update.at("old_receipt"));
                verify_update_payload(stage, update.at("kit"), update.at("receipt"));
                auto service_guard = stop_owned_service(update.at("old_receipt"), update.at("old_kit").at("version"));
                require(registry_digest(home) == update.at("registry_sha256").get<std::string>(), "registry-binding-changed", "Enrollment changed before switching kits.");
                state["update"]["step"] = "swapping"; owner->save(state);
                swap_update_payload(prefix, stage, update.at("old_receipt"), update.at("receipt"));
            } else {
                require(update.at("step") == "swapping", "installation-ownership-changed", "Unexpected payload before update activation.");
                verify_update_payload(prefix, update.at("kit"), update.at("receipt"));
                verify_update_payload(stage, update.at("old_kit"), update.at("old_receipt"));
                swap_update_payload(prefix, stage, update.at("old_receipt"), update.at("receipt"));
            }
            state["receipt"] = update.at("receipt"); state["kit"] = kit;
            state["phase"] = "activating"; owner->save(state);
        }
        require(record(prefix) == update.at("receipt"), "installation-ownership-changed", "New payload receipt changed.");
        verify_files(prefix, kit, true);
        owner.reset(); // The operation lock remains held while the new child takes its startup fence.
        resume_update_runtime(home, prefix / "bin/shin-ce", update.at("runtime"));
        owner = std::make_unique<StateStore>(control, true);
        require(owner->load() == state && registry_digest(home) == update.at("registry_sha256").get<std::string>(),
            "installation-ownership-changed", "Installation or enrollment changed during activation.");
        state["phase"] = "installed"; state.erase("update");
        state["previous"] = {{"receipt", update.at("old_receipt")}, {"kit", update.at("old_kit")}};
        owner->save(state);
        return {{"ok", true}, {"command", "update"}, {"phase", "installed"}, {"changed", true}, {"version", kit.at("version")},
            {"previous_version", update.at("old_kit").at("version")}, {"prefix", prefix.string()}, {"home", home.string()},
            {"data_preserved", true}, {"service_restored", true}, {"readiness_assessed", true}, {"previous_kit_retained", true}, {"restart_monitor", true}};
    } catch (const std::exception& error) {
        const auto* typed = dynamic_cast<const Error*>(&error);
        const auto failure = typed ? validation_identifier(typed->code) : std::string("installation-update-failed");
        try { rollback(); }
        catch (const std::exception& recovery) {
            const auto* cause = dynamic_cast<const Error*>(&recovery);
            const auto recovery_code = cause ? validation_identifier(cause->code) : std::string("installation-recovery-failed");
            throw Error("installation-update-recovery-required", "Update failed (" + failure + ") and recovery did not complete (" + recovery_code + "). Rerun update from the same extracted kit.");
        }
        throw Error("installation-update-rolled-back", "New kit activation failed (" + failure + "); the previous kit and service were restored. Project data was preserved.");
    }
}

} // namespace

std::optional<fs::path> installed_local_home(const fs::path& executable, bool startup) {
    const auto prefix = executable_prefix(executable);
    if (!present(prefix / marker_name)) return std::nullopt;
    const auto receipt = record(prefix);
    StateStore state(control_path(prefix), false);
    (void)state_record(state, receipt, true, startup);
    return fs::path(receipt.at("home").get<std::string>());
}

void check_local_installation_home(const fs::path& home, const fs::path& executable, bool startup) {
    if (!present(home / "installation")) return;
    StateStore owner(home / "installation", false);
    const auto claim = owner.load();
    const auto prefix = executable_prefix(executable);
    require(claim && claim->value("schema_version", "") == "shin.local-install-home.v1" &&
        claim->at("prefix") == prefix.string() && present(prefix / marker_name),
        "installation-home-conflict", "Use the installed shin-ce to manage this installation's data home.");
    (void)installed_local_home(executable, startup);
}

LocalInstallationGuard::~LocalInstallationGuard() { if (descriptor_ >= 0) ::close(descriptor_); }

std::unique_ptr<LocalInstallationGuard> lock_local_installation(const fs::path& executable, bool startup) {
    const auto prefix = executable_prefix(executable);
    if (!present(prefix / marker_name)) return {};
    const auto receipt = record(prefix);
    const auto path = control_path(prefix) / "observer.lock";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    require(fd >= 0, "installation-lock-unavailable", "Installation control lock is unavailable.");
    auto guard = std::make_unique<LocalInstallationGuard>(fd);
    struct stat held{}, named{};
    require(::fstat(fd, &held) == 0 && S_ISREG(held.st_mode) && held.st_nlink == 1 &&
        held.st_uid == ::geteuid() && (held.st_mode & 07777) == 0600,
        "installation-lock-unavailable", "Installation control lock is unsafe.");
    require(::flock(fd, LOCK_SH | LOCK_NB) == 0, "writer-busy", "An installation change is in progress.");
    require(::lstat(path.c_str(), &named) == 0 && held.st_dev == named.st_dev && held.st_ino == named.st_ino &&
        receipt.at("control_identity") == directory_identity(control_path(prefix), true),
        "installation-lock-unavailable", "Installation control lock changed.");
    StateStore state(control_path(prefix), false);
    const auto operation = state_record(state, receipt, true, startup);
    require(startup || !operation.contains("update"), "installation-incomplete", "An update is still restoring the service; resume it from the extracted kit.");
    return guard;
}

Json local_installation(int argc, char** argv, const fs::path& user_home) {
#ifndef __APPLE__
    throw Error("installation-platform-unsupported", "The native local kit installer is currently qualified only for macOS.");
#endif
    require(::geteuid() != 0, "installation-root-forbidden", "Install the local kit as your normal user, without sudo.");
    const std::string command = argv[1];
    const bool updating = command == "update";
    bool adopt = false;
    const auto source = executable_prefix(current_executable_path());
    fs::path prefix = present(source / marker_name) ? source : user_home / ".local/opt/shin-local";
    fs::path bin = user_home / ".local/bin";
    fs::path home = user_home / ".shin-ce";
    std::set<std::string> seen;
    for (int i = 2; i < argc; ++i) {
        const std::string name = argv[i];
        if (updating && name == "--adopt-existing" && seen.insert(name).second) { adopt = true; continue; }
        require((name == "--prefix" || ((command == "install" || updating) && (name == "--bin-dir" || name == "--home"))) &&
            seen.insert(name).second && i + 1 < argc, "invalid-arguments", "Use install/update [--prefix DIR] [--bin-dir DIR] [--home DIR], update --adopt-existing with all three paths, or installation/uninstall [--prefix DIR].");
        const auto value = normalized(argv[++i]);
        if (name == "--prefix") prefix = value;
        else if (name == "--bin-dir") bin = value;
        else home = value;
    }
    prefix = normalized(prefix); bin = normalized(bin); home = normalized(home);
    require(within(prefix, user_home) && prefix != user_home &&
        (source == prefix || (!within(source, prefix) && !within(prefix, source))),
        "installation-path-conflict", "Use an installation directory inside your home, outside the extracted kit.");
    user_directory(prefix.parent_path(), command == "install");
    const auto control = control_path(prefix);
    if (command == "install" || updating) ensure_private_directory(control);
    require(present(control), "installation-not-owned", "No owned installation exists at this prefix.");
    std::unique_ptr<StateStore> operation;
    if (command != "installation") {
        ensure_private_directory(control / "operation");
        operation = std::make_unique<StateStore>(control / "operation", true);
    }
    auto owner = std::make_unique<StateStore>(control, command != "installation");
    auto& store = *owner;
    if (command != "installation") store.recover_pending();
    auto saved = store.load();
    if (saved) require(saved->value("schema_version", "") == "shin.local-install-operation.v1" &&
        saved->at("receipt").at("prefix") == prefix.string(), "installation-state-invalid", "Installation state does not match the requested prefix.");
    if (saved) {
        (void)validate_manifest(saved->at("kit"));
        if (!seen.contains("--home")) home = saved->at("receipt").at("home").get<std::string>();
        if (!seen.contains("--bin-dir")) bin = saved->at("receipt").at("bin_dir").get<std::string>();
        require(saved->at("receipt").at("home") == home.string() && saved->at("receipt").at("bin_dir") == bin.string(),
            "installation-options-conflict", "An existing installation retains its recorded data and command directories.");
    }
    require(home == normalized(home) && bin == normalized(bin) && within(bin, user_home) && bin != user_home &&
        within(home, user_home) && home != user_home && !within(home, prefix) && !within(prefix, home) &&
        !within(bin, prefix) && !within(prefix, bin) && !within(bin, home) && !within(home, bin),
        "installation-path-conflict", "Use separate installation, command and data directories inside your home.");
    if (updating) {
        require(source != prefix, "installation-update-source", "Run update using shin-ce from the new extracted kit, outside the installed directory.");
        require(!adopt || (seen.contains("--prefix") && seen.contains("--home") && seen.contains("--bin-dir")),
            "invalid-arguments", "Adopting an existing kit requires explicit --prefix, --home and --bin-dir.");
        return update_kit(source, prefix, bin, home, adopt, owner, saved);
    }
    require(!saved || !saved->contains("update"), "installation-incomplete", "Resume the pending update from its original extracted kit.");
    if (command != "install") {
        if (!saved) throw Error("installation-not-owned", "No owned installation exists at this prefix.");
        auto state = *saved;
        const auto receipt = state.at("receipt");
        if (state.at("phase") == "removed") return {{"ok", true}, {"command", command}, {"phase", "removed"}, {"data_preserved", true}, {"prefix", prefix.string()}};
        verify_record_locations(prefix, receipt);
        require(state.at("phase") == "installed" || (command == "uninstall" && state.at("phase") == "removing"),
            "installation-incomplete", "Complete the interrupted install before uninstalling.");
        if (present(prefix)) {
            if (state.at("phase") == "installed" || present(prefix / marker_name))
                require(record(prefix) == receipt, "installation-ownership-changed", "Installation receipt changed.");
            else require(receipt.at("prefix_identity") == directory_identity(prefix, true), "installation-ownership-changed", "Removing installation directory changed.");
            if (state.at("phase") == "installed" || present(prefix / manifest_name)) require(receipt.at("manifest_sha256") == digest(read_text(prefix / manifest_name, kMaximumStateBytes, false)),
                "installation-manifest-changed", "Installed manifest changed; files were preserved.");
            if (present(prefix / manifest_name)) require(manifest(prefix) == state.at("kit"),
                "installation-manifest-changed", "Installation inventory differs from the verified manifest.");
            verify_files(prefix, state.at("kit"), true, state.at("phase") == "removing");
        } else require(state.at("phase") == "removing", "installation-missing", "Installed kit is missing.");
        if (command == "installation") {
            links(receipt, false, false);
            return {{"ok", true}, {"command", command}, {"phase", "installed"}, {"prefix", prefix.string()},
                {"home", receipt.at("home")}, {"bin_dir", receipt.at("bin_dir")}, {"integrity_verified", true},
                {"version", state.at("kit").at("version")}, {"core_version", state.at("kit").at("core_version")}};
        }
        // Fence new CLI operations before stopping the known service; data and grants remain untouched.
        links(receipt, false, false, true);
        state["phase"] = "removing"; store.save(state);
        const auto service_guard = stop_owned_service(receipt);
        links(receipt, false, true);
        if (present(prefix)) remove_payload(prefix, state.at("kit"), receipt.at("prefix_identity"));
        clear_previous(state, store, control / "update-payload");
        state["phase"] = "removed"; store.save(state);
        return {{"ok", true}, {"command", command}, {"phase", "removed"}, {"prefix", prefix.string()}, {"data_preserved", true}};
    }

    const auto kit = manifest(source);
    require(kit.at("version") == SHIN_OBSERVER_VERSION && kit.at("core_version") == SHIN_VERSION,
        "installation-version-mismatch", "Install using the shin-ce executable from this same kit.");
    verify_files(source, kit, present(source / marker_name));
    const auto manifest_bytes = read_text(source / manifest_name, kMaximumStateBytes, false);
    const auto manifest_hash = digest(manifest_bytes);
    if (saved && saved->at("phase") == "installed") {
        const auto receipt = record(prefix);
        require(receipt == saved->at("receipt") && receipt.at("manifest_sha256") == manifest_hash,
            "installation-replacement-refused", "This prefix contains another kit. In-place replacement is not supported by this installer.");
        require((!seen.contains("--home") || receipt.at("home") == home.string()) &&
            (!seen.contains("--bin-dir") || receipt.at("bin_dir") == bin.string()),
            "installation-options-conflict", "An existing installation cannot change its data or command directories.");
        verify_files(prefix, kit, true);
        links(receipt, true, false);
        return {{"ok", true}, {"command", command}, {"phase", "installed"}, {"changed", false}, {"prefix", prefix.string()}, {"home", receipt.at("home")}};
    }
    require(!present(prefix) || (saved && saved->at("phase") == "installing"),
        "installation-not-owned", "The destination already exists and is not owned by this installer; it was preserved.");
    user_directory(bin, true);
    if (!present(home)) { user_directory(home.parent_path(), true); ensure_private_directory(home); }
    const auto home_identity = directory_identity(home, true);
    const auto claim = Json{{"schema_version", "shin.local-install-home.v1"}, {"prefix", prefix.string()},
        {"control_identity", directory_identity(control, true)}};
    if (!present(home / "installation")) require(fs::is_empty(home), "installation-home-conflict", "Existing unowned project data was preserved; choose a new private data home.");
    ensure_private_directory(home / "installation");
    StateStore home_claim(home / "installation", true);
    home_claim.recover_pending();
    if (const auto existing = home_claim.load()) require(*existing == claim, "installation-home-conflict", "This data home belongs to another installation.");
    else {
        for (const auto& entry : fs::directory_iterator(home))
            require(entry.path().filename() == "installation", "installation-home-conflict", "Unclaimed data in the selected home was preserved.");
        home_claim.save(claim);
    }
    const auto stage = control / "payload";
    Json state;
    if (saved && saved->at("phase") == "installing") {
        state = *saved;
        const auto& receipt = state.at("receipt");
        require(receipt.at("home") == home.string() && receipt.at("bin_dir") == bin.string() &&
            receipt.at("home_identity") == home_identity && receipt.at("manifest_sha256") == manifest_hash,
            "installation-options-conflict", "Resume with the original kit and installation options.");
        if (present(stage)) {
            if (receipt.at("prefix_identity") == "") {
                (void)directory_identity(stage, true);
                require(fs::is_empty(stage), "installation-not-owned", "An uncommitted staging directory must be empty.");
                fs::remove(stage);
            } else remove_payload(stage, kit, receipt.at("prefix_identity"));
        }
    } else {
        require(!saved || saved->at("phase") == "removed", "installation-incomplete", "Finish the pending uninstall before installing.");
        require(!present(stage), "installation-not-owned", "An unowned staging directory was preserved.");
    }
    if (!present(prefix)) {
        auto receipt = Json{{"schema_version", "shin.local-installation.v1"}, {"prefix", prefix.string()},
            {"prefix_identity", ""}, {"control", control.string()},
            {"control_identity", directory_identity(control, true)}, {"home", home.string()}, {"home_identity", home_identity},
            {"bin_dir", bin.string()}, {"bin_dir_identity", directory_identity(bin)}, {"manifest_sha256", manifest_hash}};
        state = {{"schema_version", "shin.local-install-operation.v1"}, {"phase", "installing"}, {"receipt", receipt}, {"kit", kit}};
        store.save(state);
        ensure_private_directory(stage);
        receipt["prefix_identity"] = directory_identity(stage, true);
        state["receipt"] = receipt;
        store.save(state);
        // Preflight all command names before copying or publishing any payload.
        for (const auto* name : commands) {
            const auto link = bin / name;
            require(!present(link) || (fs::is_symlink(fs::symlink_status(link)) && fs::read_symlink(link) == prefix / "bin" / name),
                "installation-command-conflict", "An existing command is not owned by this installation; it was preserved.");
        }
        for (const auto& path : directories(kit)) ensure_private_directory(stage / path);
        for (const auto& file : kit.at("files")) {
            const auto name = file.at("path").get<std::string>();
            const auto bytes = read_text(source / name, max_file, false);
            require(bytes.size() == file.at("size") && file.at("sha256") == digest(bytes), "installation-file-changed", "Source kit changed during installation.");
            write_payload(stage / name, bytes, file.at("executable"));
        }
        write_payload(stage / manifest_name, manifest_bytes, false);
        (void)write_private_archive(stage / marker_name, receipt.dump());
        verify_files(stage, kit, true);
        for (const auto& path : directories(kit)) sync_directory(stage / path);
        sync_directory(stage);
        require(!present(prefix), "installation-not-owned", "Destination appeared during installation; it was preserved.");
#ifdef __APPLE__
        require(::renamex_np(stage.c_str(), prefix.c_str(), RENAME_EXCL) == 0,
            "installation-publish-failed", "Cannot publish the kit without replacing another directory.");
#endif
        sync_directory(prefix.parent_path());
    }
    require(record(prefix) == state.at("receipt"), "installation-ownership-changed", "Published installation changed.");
    verify_files(prefix, kit, true);
    links(state.at("receipt"), true, false);
    sync_directory(bin);
    state["phase"] = "installed"; store.save(state);
    return {{"ok", true}, {"command", command}, {"phase", "installed"}, {"changed", true}, {"prefix", prefix.string()},
        {"home", home.string()}, {"bin_dir", bin.string()}, {"version", kit.at("version")},
        {"client_settings_changed", false}, {"autostart_installed", false}, {"next_command", "shin-ce init"}};
}
} // namespace shin::observer
