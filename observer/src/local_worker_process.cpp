#include "shin/observer/local_worker_process.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#elif defined(__linux__)
#include <time.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include "shin/windows/transport.h"
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/resource.h>
#elif defined(__linux__)
#include <fstream>
#endif
#endif

namespace shin::observer {
namespace {

bool valid(const SleepSample& sample) noexcept {
    return std::isfinite(sample.lower_ms) && std::isfinite(sample.upper_ms) &&
        sample.lower_ms >= 0.0 && sample.upper_ms >= sample.lower_ms;
}

#if defined(__APPLE__)
double ticks_to_ms(std::uint64_t ticks, const mach_timebase_info_data_t& timebase) noexcept {
    if (timebase.denom == 0) return 0.0;
    const long double nanos = static_cast<long double>(ticks) * timebase.numer / timebase.denom;
    return static_cast<double>(nanos / 1'000'000.0L);
}
#elif defined(__linux__)
bool clock_ms(clockid_t id, double& result) noexcept {
    timespec value{};
    if (clock_gettime(id, &value) != 0) return false;
    result = static_cast<double>(value.tv_sec) * 1000.0 +
        static_cast<double>(value.tv_nsec) / 1'000'000.0;
    return std::isfinite(result);
}
#endif

} // namespace

std::optional<SleepSample> native_suspend_sample() noexcept {
#if defined(__APPLE__)
    // Apple documents mach_continuous_time as advancing during sleep;
    // mach_absolute_time does not. Sandwiching the continuous read between
    // absolute reads bounds call scheduling and execution skew.
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0) return std::nullopt;
    const auto absolute_before = mach_absolute_time();
    const auto continuous = mach_continuous_time();
    const auto absolute_after = mach_absolute_time();
    if (absolute_after < absolute_before) return std::nullopt;

    const double lower = continuous > absolute_after
        ? ticks_to_ms(continuous - absolute_after, timebase) : 0.0;
    const double upper = continuous > absolute_before
        ? ticks_to_ms(continuous - absolute_before, timebase) : 0.0;
    SleepSample sample{lower, upper};
    return valid(sample) ? std::optional<SleepSample>(sample) : std::nullopt;
#elif defined(__linux__) && defined(CLOCK_BOOTTIME) && defined(CLOCK_MONOTONIC)
    // CLOCK_BOOTTIME includes suspend; CLOCK_MONOTONIC excludes it. The
    // sandwich bounds the offset despite ordinary scheduling delay.
    double monotonic_before{};
    double boottime{};
    double monotonic_after{};
    if (!clock_ms(CLOCK_MONOTONIC, monotonic_before) ||
        !clock_ms(CLOCK_BOOTTIME, boottime) ||
        !clock_ms(CLOCK_MONOTONIC, monotonic_after) || monotonic_after < monotonic_before) {
        return std::nullopt;
    }
    SleepSample sample{
        std::max(0.0, boottime - monotonic_after),
        std::max(0.0, boottime - monotonic_before)};
    return valid(sample) ? std::optional<SleepSample>(sample) : std::nullopt;
#elif defined(_WIN32)
    // GetTickCount64 includes time spent asleep; QueryUnbiasedInterruptTime
    // excludes it. Both are elapsed-time counters. Their documented clock
    // granularity warrants a conservative 20 ms allowance on each bound.
    constexpr double clock_allowance_ms = 20.0;
    const ULONGLONG tick_before = GetTickCount64();
    ULONGLONG unbiased_100ns{};
    if (!QueryUnbiasedInterruptTime(&unbiased_100ns)) return std::nullopt;
    const ULONGLONG tick_after = GetTickCount64();
    if (tick_after < tick_before) return std::nullopt;

    const double unbiased_ms = static_cast<double>(unbiased_100ns) / 10'000.0;
    SleepSample sample{
        std::max(0.0, static_cast<double>(tick_before) - unbiased_ms - clock_allowance_ms),
        static_cast<double>(tick_after) - unbiased_ms + clock_allowance_ms};
    return valid(sample) ? std::optional<SleepSample>(sample) : std::nullopt;
#else
    return std::nullopt;
#endif
}

LocalSuspendObservation::LocalSuspendObservation(Sampler sampler)
    : sampler_(std::move(sampler)) {
    if (!sampler_) return;
    try {
        auto sample = sampler_();
        if (sample && valid(*sample)) baseline_ = last_ = *sample;
    } catch (...) {
        // Unsupported or failing native clock reads are treated as unavailable.
        baseline_.reset();
        last_.reset();
    }
}

double LocalSuspendObservation::suspended_ms() noexcept {
    if (!sampler_ || !baseline_) return observed_ms_;
    try {
        auto sample = sampler_();
        if (!sample || !valid(*sample)) return observed_ms_;
        if (last_ && sample->upper_ms < last_->lower_ms) return observed_ms_;
        last_ = *sample;
        observed_ms_ = std::max(observed_ms_, std::max(0.0, sample->lower_ms - baseline_->upper_ms));
    } catch (...) {
        // A transient measurement failure does not invalidate prior evidence.
        return observed_ms_;
    }
    return observed_ms_;
}

} // namespace shin::observer

namespace shin::observer {
namespace {
using Clock = LocalWorkerProcess::Clock;
constexpr std::size_t output_limit = 128 * 1024 + 4096;
void active(Clock::time_point deadline, const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) throw Error("service-request-cancelled", "Local request was cancelled.");
    if (Clock::now() >= deadline) throw Error("service-timeout", "Worker deadline expired.");
}
#ifndef _WIN32
void flags(int fd) {
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
        throw Error("service-io", "Cannot configure private worker channel.");
#ifdef __APPLE__
    const int yes = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) != 0)
        throw Error("service-io", "Cannot configure worker signal safety.");
#endif
}
struct Pair {
    int fd[2]{-1, -1};
    Pair() {
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) != 0)
            throw Error("service-worker-failed", "Cannot create private worker channel.");
    }
    ~Pair() { for (const auto value : fd) if (value >= 0) ::close(value); }
    Pair(const Pair&) = delete;
    Pair& operator=(const Pair&) = delete;
    void configure() { for (const auto value : fd) flags(value); }
    void close_child() { ::close(fd[1]); fd[1] = -1; }
};
#endif
}
class LocalWorkerProcess::Impl {
public:
#ifdef _WIN32
    shin::windows::Worker child;
    Impl(const fs::path& executable, const char* role) : child(executable, role) {}
    std::optional<std::string> read(Clock::time_point deadline) { return child.output().read_some(deadline); }
    void send(const std::string& frame, Clock::time_point deadline, const std::function<bool()>& cancelled) {
        child.input().write_frame(frame, 16384, deadline, cancelled);
    }
    void stop() noexcept { child.stop(); }
    std::uint64_t id() const { return child.process_id(); }
    std::optional<std::uint64_t> rss() const { return child.rss_bytes(); }
#else
    Pair channel;
    pid_t pid = -1;
    Impl(const fs::path& executable, const char* role) {
        channel.configure();
        posix_spawn_file_actions_t actions;
        if (::posix_spawn_file_actions_init(&actions) != 0)
            throw Error("service-worker-failed", "Cannot initialize worker actions.");
        const auto path = executable.string();
        std::array<char*, 3> arguments{const_cast<char*>(path.c_str()), const_cast<char*>(role), nullptr};
        std::array<char*, 2> environment{const_cast<char*>("LC_ALL=C"), nullptr};
        int code = ::posix_spawn_file_actions_adddup2(&actions, channel.fd[1], STDIN_FILENO);
        if (code == 0) code = ::posix_spawn_file_actions_adddup2(&actions, channel.fd[1], STDOUT_FILENO);
        if (code == 0) code = ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        if (code == 0) code = ::posix_spawn(&pid, path.c_str(), &actions, nullptr, arguments.data(), environment.data());
        ::posix_spawn_file_actions_destroy(&actions);
        if (code != 0) throw Error("service-worker-failed", "Cannot start the native worker.");
        channel.close_child();
    }
    ~Impl() { stop(); }
    void stop() noexcept {
        if (pid <= 0) return;
        // An unreaped direct child remains owned; never act on a persisted PID.
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        pid = -1;
    }
    std::uint64_t id() const { return pid > 0 ? static_cast<std::uint64_t>(pid) : 0; }
    std::optional<std::uint64_t> rss() const {
        if (pid <= 0) return {};
#ifdef __APPLE__
        rusage_info_v4 usage{};
        if (::proc_pid_rusage(pid, RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&usage)) == 0)
            return usage.ri_resident_size;
#elif defined(__linux__)
        std::ifstream stat("/proc/" + std::to_string(pid) + "/statm");
        std::uint64_t virtual_pages = 0, pages = 0;
        const auto page = ::sysconf(_SC_PAGESIZE);
        if (page > 0 && stat >> virtual_pages >> pages) return pages * static_cast<std::uint64_t>(page);
#endif
        return {};
    }
    std::optional<std::string> read(Clock::time_point deadline) {
        std::array<char, 4096> bytes{};
        for (;;) {
            const auto count = ::recv(channel.fd[0], bytes.data(), bytes.size(), 0);
            if (count >= 0) return std::string(bytes.data(), static_cast<std::size_t>(count));
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                throw Error("service-disconnected", "Worker output failed.");
            if (Clock::now() >= deadline) return {};
            pollfd item{channel.fd[0], POLLIN, 0};
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (::poll(&item, 1, static_cast<int>(std::clamp<std::int64_t>(left, 1, 20))) < 0 && errno != EINTR)
                throw Error("service-io", "Cannot wait for worker output.");
        }
    }
    void send(const std::string& payload, Clock::time_point deadline, const std::function<bool()>& cancelled) {
        const auto frame = payload + '\n';
        std::size_t sent = 0;
        while (sent < frame.size()) {
            active(deadline, cancelled);
#ifdef MSG_NOSIGNAL
            const auto count = ::send(channel.fd[0], frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
#else
            const auto count = ::send(channel.fd[0], frame.data() + sent, frame.size() - sent, 0);
#endif
            if (count > 0) sent += static_cast<std::size_t>(count);
            else if (count < 0 && errno == EINTR) continue;
            else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd item{channel.fd[0], POLLOUT, 0};
                if (::poll(&item, 1, 20) < 0 && errno != EINTR) throw Error("service-io", "Cannot wait for worker input.");
            } else throw Error("service-disconnected", "Cannot send worker input.");
        }
    }
#endif
};
LocalWorkerProcess::LocalWorkerProcess(const fs::path& executable, LocalWorkerRole role,
                                       LocalSuspendObservation::Sampler sampler)
    : role_(role), suspension_(std::move(sampler)) {
    const auto* command = role == LocalWorkerRole::resident ? "_resident-worker" :
        role == LocalWorkerRole::refresh ? "_refresh-worker" : "_service-worker";
    try { impl_ = std::make_unique<Impl>(executable, command); }
#ifdef _WIN32
    catch (const shin::windows::TransportError& error) { throw Error(error.code, error.what()); }
#endif
    catch (...) { throw; }
}
LocalWorkerProcess::~LocalWorkerProcess() = default;
void LocalWorkerProcess::stop() noexcept { impl_->stop(); }
std::uint64_t LocalWorkerProcess::process_id() const { return impl_->id(); }
std::optional<std::uint64_t> LocalWorkerProcess::rss_bytes() const { return impl_->rss(); }
bool LocalWorkerProcess::cancelled_or_suspended(const std::function<bool()>& cancelled) {
    // Check before the elapsed deadline and before pumping clients after wake.
    // An interrupted qualification is discarded and retried through normal admission.
    if (role_ == LocalWorkerRole::refresh &&
        suspension_.suspended_ms() >= LocalSuspendObservation::meaningful_sleep_threshold_ms)
        throw Error("refresh-suspended", "Refresh was interrupted by system sleep; automatic qualification will retry after wake.");
    return cancelled && cancelled();
}
void LocalWorkerProcess::send(const Json& payload, Clock::time_point deadline, const std::function<bool()>& cancelled) {
    const auto checked = [&] { return cancelled_or_suspended(cancelled); };
    active(deadline, checked);
    const auto frame = payload.dump();
    if (frame.size() >= 16384) throw Error("service-frame-too-large", "Worker request exceeds its bound.");
    try { impl_->send(frame, deadline, checked); }
#ifdef _WIN32
    catch (const shin::windows::TransportError& error) { throw Error(error.code, error.what()); }
#endif
    catch (...) { throw; }
    active(deadline, checked);
}
LocalWorkerOutput LocalWorkerProcess::read(Clock::time_point deadline) {
    try {
        // Bound total buffered output and work per pump, even for a bad worker.
        while (!ended_ && buffered_.find('\n') == std::string::npos) {
            const auto bytes = impl_->read(deadline);
            if (!bytes) break;
            if (bytes->empty()) { ended_ = true; break; }
            buffered_ += *bytes;
            const auto limit = role_ == LocalWorkerRole::resident ? output_limit : 128 * 1024;
            if (buffered_.size() > limit) throw Error("service-frame-too-large", "Worker output exceeds its bound.");
        }
    }
#ifdef _WIN32
    catch (const shin::windows::TransportError& error) { throw Error(error.code, error.what()); }
#endif
    catch (...) { throw; }
    LocalWorkerOutput result{{}, ended_};
    const auto end = buffered_.find('\n');
    if (end != std::string::npos) {
        if (role_ == LocalWorkerRole::read && end + 1 != buffered_.size())
            throw Error("service-frame-invalid", "One-shot worker accepts only one response frame.");
        result.frame = buffered_.substr(0, end);
        buffered_.erase(0, end + 1);
    }
    return result;
}
LocalWorkerOutput LocalWorkerProcess::poll() { return read(Clock::now()); }
Json LocalWorkerProcess::receive(Clock::time_point deadline, const std::function<bool()>& cancelled,
                                 const std::function<void(const Json&)>& progress) {
    const auto checked = [&] { return cancelled_or_suspended(cancelled); };
    std::size_t progress_frames = 0;
    for (;;) {
        active(deadline, checked);
        const auto output = read(std::min(deadline, Clock::now() + std::chrono::milliseconds(20)));
        active(deadline, checked);
        if (output.frame) {
            if (output.frame->size() + 1 > 128 * 1024) throw Error("service-frame-too-large", "Worker reply exceeds its bound.");
            auto value = parse_json(*output.frame);
            if (role_ == LocalWorkerRole::refresh && value.is_object() && value.value("kind", "") == "progress") {
                if (++progress_frames > 32)
                    throw Error("service-frame-invalid", "Refresh worker exceeded its progress-frame bound.");
                if (progress) progress(value);
                continue;
            }
            if (role_ == LocalWorkerRole::refresh && !buffered_.empty())
                throw Error("service-frame-invalid", "Refresh final frame must be the only trailing worker output.");
            return value;
        }
        if (output.ended) throw Error("service-disconnected", "Worker closed before its reply.");
    }
}
}
