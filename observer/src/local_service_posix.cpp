#include "shin/observer/local_service.h"
#include "shin/observer/local_service_platform.h"
#include "shin/observer/local_worker.h"
#include "shin/observer/local_rpc.h"
#include "context_internal.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>

namespace shin::observer {
namespace {
using Clock = std::chrono::steady_clock;
using local_rpc::failure;
constexpr std::size_t request_limit = 8192, response_limit = 128 * 1024;
class Fd {
public:
    explicit Fd(int descriptor = -1) : value(descriptor) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value; }
private:
    int value;
};

class ChildReaping {
public:
    ChildReaping() : previous(std::signal(SIGCHLD, SIG_DFL)) {
        if (previous == SIG_ERR) throw Error("service-worker-failed", "Cannot establish native child ownership.");
    }
    ~ChildReaping() { std::signal(SIGCHLD, previous); }
    ChildReaping(const ChildReaping&) = delete;
    ChildReaping& operator=(const ChildReaping&) = delete;
private:
    void (*previous)(int);
};

void socket_flags(int fd) {
    if (fd < 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
        throw Error("service-io", "Cannot configure the local socket.");
#ifdef __APPLE__
    const int yes = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) != 0)
        throw Error("service-io", "Cannot configure local socket signal safety.");
#endif
}

ssize_t send_bytes(int fd, const char* data, std::size_t size) {
#ifdef MSG_NOSIGNAL
    return ::send(fd, data, size, MSG_NOSIGNAL);
#else
    return ::send(fd, data, size, 0);
#endif
}

void require_not_cancelled(const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) throw Error("service-request-cancelled", "Local request was cancelled.");
}

void wait_io(int fd, short events, Clock::time_point deadline, const std::function<bool()>& cancelled = {}) {
    while (Clock::now() < deadline) {
        require_not_cancelled(cancelled);
        pollfd item{fd, events, 0};
        const int ready = ::poll(&item, 1, 20);
        if (ready > 0) {
            if (item.revents & events) return;
            throw Error("service-disconnected", "The local service connection closed.");
        }
        if (ready < 0 && errno != EINTR) throw Error("service-io", "Local socket polling failed.");
    }
    throw Error("service-timeout", "The bounded local request deadline expired.");
}

void write_frame(int fd, const std::string& bytes, Clock::time_point deadline, const std::function<bool()>& cancelled = {}) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        require_not_cancelled(cancelled);
        const auto count = send_bytes(fd, bytes.data() + sent, bytes.size() - sent);
        if (count > 0) sent += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) wait_io(fd, POLLOUT, deadline, cancelled);
        else throw Error("service-disconnected", "Cannot send a local service frame.");
        if (Clock::now() >= deadline) throw Error("service-timeout", "Local send deadline expired.");
    }
}

std::string read_frame(int fd, std::size_t limit, Clock::time_point deadline, const std::function<bool()>& cancelled = {}) {
    std::string bytes;
    std::array<char, 4096> buffer{};
    for (;;) {
        wait_io(fd, POLLIN, deadline, cancelled);
        const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) throw Error("service-disconnected", "Local response ended before its frame.");
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
        if (bytes.size() > limit) throw Error("service-frame-too-large", "Local frame exceeds its byte limit.");
        const auto end = bytes.find('\n');
        if (end == std::string::npos) continue;
        if (end != bytes.size() - 1) throw Error("service-frame-invalid", "Only one JSON frame is allowed per connection.");
        bytes.pop_back();
        return bytes;
    }
}

void require_peer(int fd) {
#ifdef __APPLE__
    uid_t user = 0;
    gid_t group = 0;
    if (::getpeereid(fd, &user, &group) != 0 || user != ::geteuid())
#else
    struct ucred peer{};
    socklen_t size = sizeof(peer);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::geteuid())
#endif
        throw Error("service-peer-denied", "Local service requires the same OS user.");
}

sockaddr_un address(const fs::path& path) {
    sockaddr_un value{};
    value.sun_family = AF_UNIX;
    const auto text = path.string();
    if (text.size() >= sizeof(value.sun_path))
        throw Error("service-path-too-long", "Use a shorter private home for the platform's Unix socket path limit.");
    std::memcpy(value.sun_path, text.c_str(), text.size() + 1);
    return value;
}

std::string socket_identity(const fs::path& path) {
    struct stat value{};
    if (::lstat(path.c_str(), &value) != 0) {
        if (errno == ENOENT) throw Error("service-unavailable", "Local service endpoint is absent.");
        throw Error("service-socket-unsafe", "Cannot inspect the local endpoint safely.");
    }
    if (!S_ISSOCK(value.st_mode) || value.st_uid != ::geteuid() ||
        (value.st_mode & 0777) != 0600 || value.st_nlink != 1)
        throw Error("service-socket-unsafe", "Service endpoint is not an owned private socket.");
    return std::to_string(value.st_dev) + ":" + std::to_string(value.st_ino);
}

std::string executable_identity(const fs::path& path) {
    struct stat value{};
    if (::lstat(path.c_str(), &value) != 0 || !S_ISREG(value.st_mode))
        throw Error("service-executable-changed", "Native worker executable is unavailable.");
    return std::to_string(value.st_dev) + ":" + std::to_string(value.st_ino) + ":" + std::to_string(value.st_size);
}



class PosixChannel final : public ServiceChannel {
public:
    explicit PosixChannel(int value) : socket(value) { socket_flags(value); }
    int fd() const { return socket.get(); }
    std::optional<std::string> read(ServiceDeadline deadline) override {
        std::array<char, 4096> bytes{};
        for (;;) {
            const auto count = ::recv(fd(), bytes.data(), bytes.size(), 0);
            if (count >= 0) return std::string(bytes.data(), static_cast<std::size_t>(count));
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) throw Error("service-disconnected", "Local read failed.");
            if (Clock::now() >= deadline) return {};
            pollfd item{fd(), POLLIN, 0};
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (::poll(&item, 1, static_cast<int>(std::clamp<std::int64_t>(left, 1, 20))) < 0 && errno != EINTR)
                throw Error("service-io", "Local polling failed.");
        }
    }
    std::optional<std::size_t> write(std::string_view bytes) override {
        const auto count = send_bytes(fd(), bytes.data(), bytes.size());
        if (count >= 0) return static_cast<std::size_t>(count);
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return {};
        throw Error("service-disconnected", "Local write failed.");
    }
    void wait_write(ServiceDeadline deadline) override {
        try { wait_io(fd(), POLLOUT, deadline); }
        catch (const Error& error) { if (error.code != "service-timeout") throw; }
    }
    bool input_or_closed() const override {
        char byte = 0;
        const auto count = ::recv(fd(), &byte, 1, MSG_PEEK);
        return count >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
    }
    bool reply_consumed() const override { return true; }
    void finish_reply(ServiceDeadline, const ServiceCancelled&) override {}
private:
    Fd socket;
};
class PosixEndpoint final : public ServiceEndpoint {
public:
    explicit PosixEndpoint(const fs::path& home)
        : directory(local_service_directory(home)), path(directory / "rpc.sock"), listener(::socket(AF_UNIX, SOCK_STREAM, 0)) {
        socket_flags(listener.get());
        directory_id = directory_identity(directory, true);
        const auto endpoint = address(path);
        struct stat existing{};
        if (::lstat(path.c_str(), &existing) == 0) {
            (void)socket_identity(path);
            Fd probe(::socket(AF_UNIX, SOCK_STREAM, 0)); socket_flags(probe.get());
            if (::connect(probe.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0 || errno != ECONNREFUSED)
                throw Error("service-endpoint-busy", "Existing endpoint is not a confirmed stale service socket.");
            if (::unlink(path.c_str()) != 0) throw Error("service-io", "Cannot reclaim stale local socket.");
        } else if (errno != ENOENT) throw Error("service-io", "Cannot inspect local endpoint.");
        const auto previous = ::umask(0177);
        const int bound = ::bind(listener.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
        ::umask(previous);
        if (bound != 0) throw Error("service-bind-failed", "Cannot bind private local socket.");
        if (::chmod(path.c_str(), 0600) != 0) throw Error("service-io", "Cannot protect local endpoint.");
        socket_id = socket_identity(path);
        if (::listen(listener.get(), 8) != 0) throw Error("service-io", "Cannot listen on private local socket.");
    }
    ~PosixEndpoint() override {
        try { verify(); (void)::unlink(path.c_str()); }
        catch (...) { return; } // Lost ownership: preserve the named endpoint.
    }
    void verify() const override {
        if (directory_identity(directory, true) != directory_id || socket_identity(path) != socket_id)
            throw Error("service-endpoint-changed", "Private service endpoint identity changed.");
    }
    std::unique_ptr<ServiceChannel> accept() override {
        const int fd = ::accept(listener.get(), nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return {};
            throw Error("service-io", "Local connection acceptance failed.");
        }
        auto channel = std::make_unique<PosixChannel>(fd);
        try { require_peer(fd); } catch (const Error&) { return {}; }
        return channel;
    }
private:
    fs::path directory, path;
    Fd listener;
    std::string directory_id, socket_id;
};
class PosixLaunch final : public ServiceLaunch {
public:
    PosixLaunch(const fs::path& executable, const fs::path& home) {
        posix_spawn_file_actions_t actions;
        if (::posix_spawn_file_actions_init(&actions) != 0) throw Error("service-start-failed", "Cannot initialize service startup.");
        const auto binary = executable.string(), data = home.string();
        std::array<char*, 5> args{const_cast<char*>(binary.c_str()), const_cast<char*>("_serve-registered"),
            const_cast<char*>("--home"), const_cast<char*>(data.c_str()), nullptr};
        std::array<char*, 2> environment{const_cast<char*>("LC_ALL=C"), nullptr};
        int code = 0;
        for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO})
            if (!code) code = ::posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == STDIN_FILENO ? O_RDONLY : O_WRONLY, 0);
        if (!code) code = ::posix_spawn(&pid, binary.c_str(), &actions, nullptr, args.data(), environment.data());
        ::posix_spawn_file_actions_destroy(&actions);
        if (code) throw Error("service-start-failed", "Cannot start native local service.");
    }
    ~PosixLaunch() override {
        if (pid > 0) { ::kill(pid, SIGKILL); while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {} }
    }
    bool exited() override {
        const auto result = ::waitpid(pid, nullptr, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) { pid = -1; return true; }
        return false;
    }
    void release() override { pid = -1; }
private:
    pid_t pid = -1;
};
class PosixReaping final : public ServiceReaping { ChildReaping guard; };
} // namespace
std::uint64_t service_process_id() { return static_cast<std::uint64_t>(::getpid()); }
const char* service_transport() { return "local-socket-v1"; }
std::string service_executable_identity(const fs::path& path) { return executable_identity(path); }
std::unique_ptr<ServiceReaping> service_reaping() { return std::make_unique<PosixReaping>(); }
std::unique_ptr<ServiceEndpoint> make_service_endpoint(const fs::path& home) { return std::make_unique<PosixEndpoint>(home); }
std::unique_ptr<ServiceLaunch> launch_service(const fs::path& executable, const fs::path& home) { return std::make_unique<PosixLaunch>(executable, home); }
void check_stale_service_endpoint(const fs::path& home) {
    const auto path = local_service_directory(home) / "rpc.sock";
    if (fs::exists(fs::symlink_status(path))) (void)socket_identity(path);
}
std::unique_ptr<ServiceChannel> connect_service(const fs::path& home, ServiceDeadline deadline, const ServiceCancelled& cancelled) {
    const auto directory = local_service_directory(home);
    const auto identity = directory_identity(directory, true);
    const auto path = directory / "rpc.sock";
    const auto endpoint_identity = socket_identity(path);
    const auto endpoint = address(path);
    auto socket = std::make_unique<PosixChannel>(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (::connect(socket->fd(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
        if (errno != EINPROGRESS) throw Error("service-unavailable", "Local service is not accepting requests.");
        wait_io(socket->fd(), POLLOUT, deadline, cancelled);
        int error = 0; socklen_t size = sizeof(error);
        if (::getsockopt(socket->fd(), SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0)
            throw Error("service-unavailable", "Cannot connect to local service.");
    }
    require_peer(socket->fd());
    if (directory_identity(directory, true) != identity || socket_identity(path) != endpoint_identity)
        throw Error("service-endpoint-changed", "Private endpoint changed during connection.");
    return socket;
}
int local_refresh_worker() {
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGALRM, SIG_DFL);
    sigset_t alarm_signal;
    sigemptyset(&alarm_signal);
    sigaddset(&alarm_signal, SIGALRM);
    if (::pthread_sigmask(SIG_UNBLOCK, &alarm_signal, nullptr) != 0)
        throw Error("refresh-worker-failed", "Cannot activate refresh deadline.");
    ::alarm(60);
    Json result;
    try {
        require_peer(STDIN_FILENO);
        const auto payload = parse_json(read_frame(STDIN_FILENO, 16384, Clock::now() + std::chrono::seconds(2)));
        result = execute_local_worker_refresh(payload, [] {
            char byte = 0;
            const auto count = ::recv(STDIN_FILENO, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
            return count >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
        }, [](const Json& progress) {
            const auto frame = progress.dump() + '\n';
            write_frame(STDOUT_FILENO, frame, Clock::now() + std::chrono::seconds(2));
        });
    } catch (const Error& error) { result = {{"ok", false}, {"error", error_json(error)}}; }
    catch (const shin::context::CliError& error) { result = {{"ok", false}, {"error", {{"code", error.code()}, {"message", error.what()}}}}; }
    catch (const std::exception&) { result = {{"ok", false}, {"error", {{"code", "refresh-worker-failed"}, {"message", "Native refresh failed."}}}}; }
    auto frame = result.dump() + '\n';
    if (frame.size() > response_limit)
        frame = "{\"ok\":false,\"error\":{\"code\":\"refresh-worker-failed\",\"message\":\"Refresh response exceeds its bound.\"}}\n";
    write_frame(STDOUT_FILENO, frame, Clock::now() + std::chrono::seconds(2));
    return 0;
}

int local_resident_worker() {
    std::signal(SIGALRM, SIG_DFL);
    sigset_t alarm_signal;
    sigemptyset(&alarm_signal);
    sigaddset(&alarm_signal, SIGALRM);
    if (::pthread_sigmask(SIG_UNBLOCK, &alarm_signal, nullptr) != 0)
        throw Error("service-worker-failed", "Cannot activate the resident deadline.");
    ::alarm(60);
    std::unique_ptr<LocalResidentWorker> resident;
    try {
        require_peer(STDIN_FILENO);
        const auto payload = parse_json(read_frame(STDIN_FILENO, request_limit, Clock::now() + std::chrono::seconds(2)));
        resident = std::make_unique<LocalResidentWorker>(payload);
        write_frame(STDOUT_FILENO, resident->ready().dump() + '\n',
            Clock::now() + std::chrono::seconds(2));
    } catch (const std::exception& error) {
        const auto* observer = dynamic_cast<const Error*>(&error);
        const auto* core = dynamic_cast<const shin::context::CliError*>(&error);
        const auto code = observer ? observer->code : core ? core->code() : "resident-preparation-failed";
        write_frame(STDOUT_FILENO, Json{{"kind", "preparation-failed"}, {"code", code}}.dump() + '\n',
            Clock::now() + std::chrono::seconds(2));
        return 1;
    }
    ::alarm(0);
    for (;;) {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        const auto available = ::poll(&input, 1, 100);
        if (available < 0 && errno == EINTR) continue;
        if (available < 0 || (input.revents & (POLLHUP | POLLERR | POLLNVAL))) return 0;
        if (available == 0) {
            if (resident->observed_unchanged()) continue;
            write_frame(STDOUT_FILENO, "{\"kind\":\"invalidated\",\"code\":\"project-updating\"}\n",
                Clock::now() + std::chrono::seconds(2));
            return 0;
        }
        ::alarm(30);
        const auto started = Clock::now();
        Json result;
        try {
            const auto frame = read_frame(STDIN_FILENO, request_limit, Clock::now() + std::chrono::seconds(2));
            result = resident->execute(frame, started);
        } catch (const Error& error) {
            const auto response = failure("", error.code, error.what());
            result = {{"kind", "response"}, {"response", response}, {"reusable", false},
                {"metric", local_worker_metric(response, std::chrono::duration<double, std::milli>(Clock::now() - started).count())}};
        }
        write_frame(STDOUT_FILENO, result.dump() + '\n', Clock::now() + std::chrono::seconds(2));
        ::alarm(0);
        if (!result.at("reusable").get<bool>()) return 0;
    }
}

int local_service_worker() {
    // Native one-shot worker: alarm bounds execution even if its parent dies.
    std::signal(SIGALRM, SIG_DFL);
    sigset_t alarm_signal;
    sigemptyset(&alarm_signal);
    sigaddset(&alarm_signal, SIGALRM);
    if (::pthread_sigmask(SIG_UNBLOCK, &alarm_signal, nullptr) != 0)
        throw Error("service-worker-failed", "Cannot activate the native worker deadline.");
    ::alarm(30);
    std::string id;
    Json response;
    try {
        const auto payload = parse_json(read_frame(STDIN_FILENO, 16384, Clock::now() + std::chrono::seconds(2)));
        response = execute_local_worker_read(payload);
    } catch (const Error& error) { response = failure(id, error.code, error.what()); }
    catch (const shin::context::CliError& error) { response = failure(id, error.code(), error.what()); }
    catch (const std::exception&) { response = failure(id, "service-request-failed", "Native project request failed."); }
    auto frame = response.dump() + '\n';
    if (frame.size() > response_limit) {
        response = failure(id, "service-frame-too-large", "Native response exceeds its byte limit.");
        frame = response.dump() + '\n';
    }
    write_frame(STDOUT_FILENO, frame, Clock::now() + std::chrono::seconds(2));
    return 0;
}
} // namespace shin::observer
