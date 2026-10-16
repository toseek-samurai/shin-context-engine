#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace shin::windows {
using Deadline = std::chrono::steady_clock::time_point;
using Cancelled = std::function<bool()>;
struct TransportError : std::runtime_error {
    std::string code;
    unsigned long native = 0;
    TransportError(const char* reason, const char* message, unsigned long error = 0) : std::runtime_error(message), code(reason), native(error) {}
};

// Private byte-stream transport; one bounded newline-terminated frame at a time.
// Calls on a channel are serialized by its owner. No TCP/remote pipe fallback.
class Channel {
public:
    ~Channel();
    Channel(Channel&&) noexcept;
    Channel& operator=(Channel&&) noexcept;
    static Channel connect(const std::string& endpoint, Deadline deadline, const Cancelled& cancelled = {});
    // Separate machine lifecycle endpoint. Verify its SYSTEM owner and SCM PID;
    // never use this cross-user channel for project data or source queries.
    static Channel connect_system(const std::string& endpoint, unsigned long server_pid, Deadline deadline);
    const std::string& peer_sid() const;
    // Only Worker-inherited overlapped stdio. Verify both private client ends
    // belong to the same live same-user parent; duplicate without inheritance.
    static std::pair<Channel, Channel> worker_stdio();
    bool input_or_closed() const;
    // Timeout leaves the pending first-byte read intact; read_frame consumes it.
    bool wait_input(Deadline deadline, const Cancelled& cancelled = {});
    // Stream reader for the service pump, separate from framed/first-byte reads.
    // Pending IO survives timeout; nullopt means pending, empty means closed.
    std::optional<std::string> read_some(Deadline deadline);
    std::optional<std::size_t> write_some(std::string_view bytes);
    bool wait_written(Deadline deadline);
    bool reply_consumed() const;
    void write_frame(std::string_view bytes, std::size_t limit, Deadline deadline, const Cancelled& cancelled = {});
    std::string read_frame(std::size_t limit, Deadline deadline, const Cancelled& cancelled = {});
    // Server retains a reply until the client closes after reading it; never an
    // unbounded FlushFileBuffers on the service thread. Extra input is refused.
    void finish_reply(Deadline deadline, const Cancelled& cancelled = {});
    void close() noexcept;
private:
    friend class PipeListener;
    friend class Worker;
    struct Impl;
    explicit Channel(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class PipeListener {
public:
    // Endpoint is a caller-derived 64-character lowercase SHA256 key, not a path.
    explicit PipeListener(const std::string& endpoint);
    static std::unique_ptr<PipeListener> system_broker(const std::string& endpoint);
    ~PipeListener();
    Channel accept(Deadline deadline, const Cancelled& cancelled = {});
    std::unique_ptr<Channel> try_accept();
private:
    friend class Worker;
    PipeListener(const std::string& endpoint, bool system);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Suspended spawn -> owned kill-on-close Job -> resume. Only stdio is inherited.
// Roles are the existing native worker entry points; executable must match this
// running image. Cancellation/destruction kills only this owned process tree.
class Worker {
public:
    Worker(const std::filesystem::path& executable, const std::string& role);
    ~Worker();
    Channel& input();
    Channel& output();
    unsigned long wait(Deadline deadline, const Cancelled& cancelled = {});
    void stop() noexcept;
    unsigned long process_id() const;
    std::optional<std::uint64_t> rss_bytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
