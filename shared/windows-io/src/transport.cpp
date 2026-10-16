#include "shin/windows/transport.h"
#include "shin/windows/file.h"
#include "internal.h"
#include <bcrypt.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace shin::windows {
namespace {
using namespace detail;
using Clock = std::chrono::steady_clock;
constexpr std::size_t maximum_frame = 128 * 1024;
void active(Deadline deadline, const Cancelled& cancelled) {
    if (cancelled && cancelled()) throw TransportError("service-request-cancelled", "Local request was cancelled.");
    if (Clock::now() >= deadline) throw TransportError("service-timeout", "Local request deadline expired.");
}
void wait_handle(HANDLE handle, Deadline deadline, const Cancelled& cancelled) {
    for (;;) {
        active(deadline, cancelled);
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const auto status = WaitForSingleObject(handle, static_cast<DWORD>(std::clamp<std::int64_t>(left, 1, 20)));
        if (status == WAIT_OBJECT_0) { active(deadline, cancelled); return; }
        if (status != WAIT_TIMEOUT) failed("Cannot wait for owned native IO");
    }
}
struct Operation {
    HANDLE pipe;
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    OVERLAPPED value{};
    bool pending = false;
    explicit Operation(HANDLE input) : pipe(input) {
        if (!event.get()) failed("Cannot create pipe IO event");
        value.hEvent = event.get();
    }
    ~Operation() {
        if (pending) {
            CancelIoEx(pipe, &value);
            DWORD ignored = 0;
            GetOverlappedResult(pipe, &value, &ignored, TRUE);
        }
    }
    DWORD finish(BOOL complete, DWORD initial, Deadline deadline, const Cancelled& cancelled) {
        if (complete) { active(deadline, cancelled); return initial; }
        const auto error = GetLastError();
        if (error != ERROR_IO_PENDING) throw TransportError("service-disconnected", "Local pipe closed or failed.", error);
        pending = true;
        wait_handle(event.get(), deadline, cancelled);
        DWORD count = 0;
        const auto ok = GetOverlappedResult(pipe, &value, &count, FALSE);
        if (!ok) throw TransportError("service-disconnected", "Local pipe operation failed.", GetLastError());
        pending = false;
        return count;
    }
};
std::wstring pipe_name(const std::string& endpoint, bool system = false) {
    require(endpoint.size() == 64 && endpoint.find_first_not_of("0123456789abcdef") == std::string::npos,
            "Invalid local endpoint key");
    return (system ? L"\\\\.\\pipe\\shin-system-v1-" : L"\\\\.\\pipe\\shin-local-v1-") + std::wstring(endpoint.begin(), endpoint.end());
}
template<class Security> Handle server_pipe(const std::wstring& name, bool first, const Security& security) {
    auto attributes = security.attributes();
    Handle handle(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
        (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0), PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        // Eight admitted clients, one being rejected at capacity, one listener.
        10, 8192, 8192, 0, &attributes));
    if (handle.get() == INVALID_HANDLE_VALUE) throw TransportError("service-endpoint-busy", "Cannot exclusively own the private pipe endpoint.");
    security.verify(handle.get());
    return handle;
}
std::vector<unsigned char> token_user(HANDLE token) {
    DWORD bytes = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    require(bytes >= sizeof(TOKEN_USER) && bytes < 65536, "Invalid broker peer token");
    std::vector<unsigned char> result(bytes);
    if (!GetTokenInformation(token, TokenUser, result.data(), bytes, &bytes)) failed("Cannot identify broker peer");
    return result;
}
class SystemSecurity {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
public:
    SystemSecurity() {
        // AU can exchange bytes, but cannot create server instances (bit 4),
        // change permissions or take ownership. Only SYSTEM owns the server.
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:SYD:P(A;;FA;;;SY)(A;;0x12019b;;;AU)", SDDL_REVISION_1, &descriptor, nullptr)) failed("Cannot create broker ACL");
    }
    ~SystemSecurity() { LocalFree(descriptor); }
    SECURITY_ATTRIBUTES attributes() const { return {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE}; }
    void verify(HANDLE handle) const {
        PSID owner = nullptr; PACL acl = nullptr; PSECURITY_DESCRIPTOR actual = nullptr;
        const auto status = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner, nullptr, &acl, nullptr, &actual);
        const LocalAllocation cleanup(actual);
        if (status != ERROR_SUCCESS) throw TransportError("service-peer-denied", "Cannot inspect broker pipe ACL.", status);
        SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision=0;
        require(GetSecurityDescriptorControl(actual,&control,&revision) && (control&SE_DACL_PROTECTED) &&
            owner && IsWellKnownSid(owner, WinLocalSystemSid) && acl && acl->AceCount == 2, "Unsafe broker pipe owner/ACL");
        for (DWORD i = 0; i < 2; ++i) {
            void* raw = nullptr; require(GetAce(acl, i, &raw) != 0, "Missing broker ACE");
            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            require(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceFlags == 0 &&
                ace->Mask == (i ? 0x12019bUL : FILE_ALL_ACCESS) &&
                IsWellKnownSid(const_cast<DWORD*>(&ace->SidStart), i ? WinAuthenticatedUserSid : WinLocalSystemSid), "Unsafe broker pipe access");
        }
    }
};
std::string system_peer(HANDLE pipe, bool server, DWORD expected_pid = 0) {
    ULONG pid = 0, verified = 0;
    const auto get = server ? GetNamedPipeClientProcessId : GetNamedPipeServerProcessId;
    if (!get(pipe, &pid) || (!server && (!expected_pid || pid != expected_pid)))
        throw TransportError("service-peer-denied", "Broker process does not match SCM.");
    // The client already checked the pipe's SYSTEM owner and exact ACL. It
    // must not need permission to open the privileged service process token.
    if (!server) return {};
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    HANDLE raw = nullptr;
    if (!process.get() || !OpenProcessToken(process.get(), TOKEN_QUERY, &raw))
        throw TransportError("service-peer-denied", "Cannot inspect broker peer.");
    Handle token(raw); const auto bytes = token_user(token.get());
    auto* sid = reinterpret_cast<const TOKEN_USER*>(bytes.data())->User.Sid;
    if (!get(pipe, &verified) || verified != pid ||
        WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT)
        throw TransportError("service-peer-denied", "Broker peer identity changed.");
    LPSTR text = nullptr;
    if (!ConvertSidToStringSidA(sid, &text)) failed("Cannot encode broker peer");
    const LocalAllocation cleanup(text); return text;
}
void peer(HANDLE pipe, bool server, const PrivateSecurity& security) {
    ULONG pid = 0, verified = 0;
    const auto get = server ? GetNamedPipeClientProcessId : GetNamedPipeServerProcessId;
    if (!get(pipe, &pid)) throw TransportError("service-peer-denied", "Cannot identify the local pipe peer.");
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    HANDLE raw = nullptr;
    if (!process.get() || !OpenProcessToken(process.get(), TOKEN_QUERY, &raw))
        throw TransportError("service-peer-denied", "Cannot inspect the local peer owner.");
    Handle token(raw);
    if (!security.same_user(token.get()) || !get(pipe, &verified) || verified != pid || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT)
        throw TransportError("service-peer-denied", "Local pipe requires the same live OS user.");
}
Handle client_pipe(const std::wstring& name, bool asynchronous) {
    // Identification only: a pipe server must not impersonate the client.
    return Handle(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL, 0, nullptr, OPEN_EXISTING,
        (asynchronous ? FILE_FLAG_OVERLAPPED : 0) | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
}
std::string random_key() {
    std::array<unsigned char, 32> bytes{};
    require(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0,
            "Cannot generate private worker channel");
    std::string result;
    for (const auto byte : bytes) { result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15]; }
    return result;
}
}

struct Channel::Impl {
    detail::Handle handle;
    std::string peer;
    char first = 0;
    bool buffered = false;
    std::unique_ptr<Operation> readiness;
    std::array<char, 4096> stream_buffer{};
    std::unique_ptr<Operation> stream_read;
    std::string write_buffer;
    std::unique_ptr<Operation> stream_write;
    explicit Impl(detail::Handle input) : handle(std::move(input)) {}
};
Channel::Channel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Channel::~Channel() = default;
const std::string& Channel::peer_sid() const { return impl_->peer; }
void Channel::close() noexcept {
    if (impl_) {
        impl_->readiness.reset();
        impl_->stream_read.reset();
        impl_->stream_write.reset();
        impl_->buffered = false;
        impl_->handle = detail::Handle();
    }
}
Channel::Channel(Channel&&) noexcept = default;
Channel& Channel::operator=(Channel&&) noexcept = default;
std::pair<Channel, Channel> Channel::worker_stdio() {
    PrivateSecurity security;
    ULONG parent = 0;
    const auto take = [&](DWORD slot) {
        const auto raw = GetStdHandle(slot);
        DWORD flags = 0; ULONG pid = 0;
        if (!raw || raw == INVALID_HANDLE_VALUE || GetFileType(raw) != FILE_TYPE_PIPE ||
            !GetNamedPipeInfo(raw, &flags, nullptr, nullptr, nullptr) || (flags & PIPE_SERVER_END) ||
            !GetNamedPipeServerProcessId(raw, &pid) || pid == GetCurrentProcessId() || (parent && pid != parent))
            throw TransportError("service-peer-denied", "Worker requires inherited private parent channels.");
        security.verify(raw); peer(raw, false, security); parent = pid;
        HANDLE copy = nullptr;
        if (!SetHandleInformation(raw, HANDLE_FLAG_INHERIT, 0) ||
            !DuplicateHandle(GetCurrentProcess(), raw, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
            failed("Cannot retain private worker channel");
        return Channel(std::make_unique<Impl>(Handle(copy)));
    };
    auto input = take(STD_INPUT_HANDLE);
    auto output = take(STD_OUTPUT_HANDLE);
    return {std::move(input), std::move(output)};
}
bool Channel::input_or_closed() const {
    if (impl_->buffered || (impl_->readiness && WaitForSingleObject(impl_->readiness->event.get(), 0) == WAIT_OBJECT_0)) return true;
    DWORD bytes = 0;
    return !PeekNamedPipe(impl_->handle.get(), nullptr, 0, nullptr, &bytes, nullptr) || bytes != 0;
}
bool Channel::wait_input(Deadline deadline, const Cancelled& cancelled) {
    if (impl_->stream_read) throw TransportError("service-frame-invalid", "Cannot mix native stream and framed reads.");
    if (cancelled && cancelled()) throw TransportError("service-request-cancelled", "Local request was cancelled.");
    if (impl_->buffered) return true;
    if (!impl_->readiness) {
        impl_->readiness = std::make_unique<Operation>(impl_->handle.get());
        DWORD count = 0;
        const auto ok = ::ReadFile(impl_->handle.get(), &impl_->first, 1, &count, &impl_->readiness->value);
        if (ok) {
            impl_->readiness.reset();
            if (!count) throw TransportError("service-disconnected", "Local pipe closed before its frame.");
            impl_->buffered = true;
            return true;
        }
        const auto error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            impl_->readiness.reset();
            throw TransportError("service-disconnected", "Local pipe closed or failed.", error);
        }
        impl_->readiness->pending = true;
    }
    try { wait_handle(impl_->readiness->event.get(), deadline, cancelled); }
    catch (const TransportError& error) {
        if (error.code == "service-timeout") return false;
        throw;
    }
    DWORD count = 0;
    const auto ok = GetOverlappedResult(impl_->handle.get(), &impl_->readiness->value, &count, FALSE);
    const auto error = ok ? ERROR_SUCCESS : GetLastError();
    impl_->readiness->pending = false;
    impl_->readiness.reset();
    if (!ok || !count) throw TransportError("service-disconnected", "Local pipe closed before its frame.", error);
    impl_->buffered = true;
    return true;
}
std::optional<std::string> Channel::read_some(Deadline deadline) {
    if (impl_->readiness || impl_->buffered) throw TransportError("service-frame-invalid", "Cannot mix native stream and framed reads.");
    DWORD count = 0;
    if (!impl_->stream_read) {
        impl_->stream_read = std::make_unique<Operation>(impl_->handle.get());
        const auto ok = ::ReadFile(impl_->handle.get(), impl_->stream_buffer.data(),
            static_cast<DWORD>(impl_->stream_buffer.size()), &count, &impl_->stream_read->value);
        if (ok) { impl_->stream_read.reset(); return std::string(impl_->stream_buffer.data(), count); }
        const auto error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            impl_->stream_read.reset();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) return std::string{};
            throw TransportError("service-disconnected", "Local stream read failed.", error);
        }
        impl_->stream_read->pending = true;
    }
    auto status = WaitForSingleObject(impl_->stream_read->event.get(), 0);
    if (status == WAIT_TIMEOUT && Clock::now() < deadline) {
        try { wait_handle(impl_->stream_read->event.get(), deadline, {}); }
        catch (const TransportError& error) {
            if (error.code == "service-timeout") return {};
            throw;
        }
        status = WAIT_OBJECT_0;
    }
    if (status == WAIT_TIMEOUT) return {};
    if (status != WAIT_OBJECT_0) failed("Cannot poll native stream read");
    const auto ok = GetOverlappedResult(impl_->handle.get(), &impl_->stream_read->value, &count, FALSE);
    const auto error = ok ? ERROR_SUCCESS : GetLastError();
    impl_->stream_read->pending = false;
    impl_->stream_read.reset();
    if (!ok && error != ERROR_BROKEN_PIPE && error != ERROR_PIPE_NOT_CONNECTED)
        throw TransportError("service-disconnected", "Local stream read failed.", error);
    return ok ? std::string(impl_->stream_buffer.data(), count) : std::string{};
}
Channel Channel::connect(const std::string& endpoint, Deadline deadline, const Cancelled& cancelled) {
    const auto name = pipe_name(endpoint);
    PrivateSecurity security;
    for (;;) {
        active(deadline, cancelled);
        auto file = client_pipe(name, true);
        if (file.get() != INVALID_HANDLE_VALUE) {
            security.verify(file.get()); peer(file.get(), false, security);
            active(deadline, cancelled);
            return Channel(std::make_unique<Impl>(std::move(file)));
        }
        if (GetLastError() != ERROR_PIPE_BUSY) throw TransportError("service-unavailable", "Private pipe endpoint is unavailable.");
        // WaitNamedPipe is bounded; cancellation is checked at each interval.
        WaitNamedPipeW(name.c_str(), 20);
    }
}
Channel Channel::connect_system(const std::string& endpoint, unsigned long server_pid, Deadline deadline) {
    SystemSecurity security;
    const auto name = pipe_name(endpoint, true);
    for (;;) {
        active(deadline, {});
        Handle file(CreateFileW(name.c_str(), FILE_READ_DATA | FILE_WRITE_DATA | READ_CONTROL | SYNCHRONIZE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (file.get() != INVALID_HANDLE_VALUE) {
            security.verify(file.get()); (void)system_peer(file.get(), false, server_pid);
            return Channel(std::make_unique<Impl>(std::move(file)));
        }
        if (GetLastError() != ERROR_PIPE_BUSY) throw TransportError("service-unavailable", "SCM host endpoint is unavailable.");
        WaitNamedPipeW(name.c_str(), 20);
    }
}
std::optional<std::size_t> Channel::write_some(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > maximum_frame) throw TransportError("service-frame-too-large", "Invalid stream write size.");
    DWORD count = 0;
    if (!impl_->stream_write) {
        impl_->write_buffer = bytes;
        impl_->stream_write = std::make_unique<Operation>(impl_->handle.get());
        const auto ok = WriteFile(impl_->handle.get(), impl_->write_buffer.data(),
            static_cast<DWORD>(impl_->write_buffer.size()), &count, &impl_->stream_write->value);
        if (ok) { impl_->stream_write.reset(); impl_->write_buffer.clear(); return count; }
        const auto error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            impl_->stream_write.reset(); impl_->write_buffer.clear();
            throw TransportError("service-disconnected", "Local stream write failed.", error);
        }
        impl_->stream_write->pending = true;
    } else if (bytes != impl_->write_buffer) throw TransportError("service-frame-invalid", "Pending stream write changed.");
    const auto state = WaitForSingleObject(impl_->stream_write->event.get(), 0);
    if (state == WAIT_TIMEOUT) return {};
    if (state != WAIT_OBJECT_0) failed("Cannot poll stream write");
    const auto ok = GetOverlappedResult(impl_->handle.get(), &impl_->stream_write->value, &count, FALSE);
    const auto error = ok ? ERROR_SUCCESS : GetLastError();
    impl_->stream_write->pending = false;
    impl_->stream_write.reset(); impl_->write_buffer.clear();
    if (!ok || !count) throw TransportError("service-disconnected", "Local stream write failed.", error);
    return count;
}
bool Channel::wait_written(Deadline deadline) {
    if (!impl_->stream_write) return true;
    try { wait_handle(impl_->stream_write->event.get(), deadline, {}); return true; }
    catch (const TransportError& error) { if (error.code == "service-timeout") return false; throw; }
}
bool Channel::reply_consumed() const {
    DWORD bytes = 0;
    if (!PeekNamedPipe(impl_->handle.get(), nullptr, 0, nullptr, &bytes, nullptr)) {
        const auto error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) return true;
        throw TransportError("service-disconnected", "Cannot check reply consumption.", error);
    }
    if (bytes) throw TransportError("service-frame-invalid", "Unexpected input after the local request.");
    return false;
}
void Channel::write_frame(std::string_view bytes, std::size_t limit, Deadline deadline, const Cancelled& cancelled) {
    active(deadline, cancelled);
    if (!limit || limit > maximum_frame || bytes.size() >= limit)
        throw TransportError("service-frame-too-large", "Local frame exceeds its byte limit.");
    if (bytes.find('\n') != std::string_view::npos) throw TransportError("service-frame-invalid", "Only one local frame is allowed.");
    std::string framed(bytes); framed += '\n';
    std::size_t offset = 0;
    while (offset < framed.size()) {
        active(deadline, cancelled);
        Operation op(impl_->handle.get()); DWORD count = 0;
        const auto ok = WriteFile(impl_->handle.get(), framed.data() + offset, static_cast<DWORD>(framed.size() - offset), &count, &op.value);
        count = op.finish(ok, count, deadline, cancelled);
        if (!count) throw TransportError("service-disconnected", "Local pipe write made no progress.");
        offset += count;
    }
}
std::string Channel::read_frame(std::size_t limit, Deadline deadline, const Cancelled& cancelled) {
    if (impl_->stream_read) throw TransportError("service-frame-invalid", "Cannot mix native stream and framed reads.");
    if (!limit || limit > maximum_frame) throw TransportError("service-frame-too-large", "Invalid local frame limit.");
    std::string result;
    if (impl_->buffered || impl_->readiness) {
        if (!wait_input(deadline, cancelled)) throw TransportError("service-timeout", "Local request deadline expired.");
        result += impl_->first;
        impl_->buffered = false;
        if (result.back() == '\n') return {};
    }
    std::array<char, 4096> buffer{};
    for (;;) {
        active(deadline, cancelled);
        Operation op(impl_->handle.get()); DWORD count = 0;
        const auto ok = ::ReadFile(impl_->handle.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, &op.value);
        count = op.finish(ok, count, deadline, cancelled);
        if (!count) throw TransportError("service-disconnected", "Local pipe closed before its frame.");
        result.append(buffer.data(), count);
        if (result.size() > limit) throw TransportError("service-frame-too-large", "Local frame exceeds its byte limit.");
        const auto end = result.find('\n');
        if (end == std::string::npos) continue;
        if (end + 1 != result.size()) throw TransportError("service-frame-invalid", "Only one local frame is allowed.");
        result.pop_back(); return result;
    }
}
void Channel::finish_reply(Deadline deadline, const Cancelled& cancelled) {
    active(deadline, cancelled);
    Operation op(impl_->handle.get()); char extra = 0; DWORD count = 0;
    const auto ok = ::ReadFile(impl_->handle.get(), &extra, 1, &count, &op.value);
    try { count = op.finish(ok, count, deadline, cancelled); }
    catch (const TransportError& error) {
        if (error.code == "service-disconnected" && (error.native == ERROR_BROKEN_PIPE || error.native == ERROR_PIPE_NOT_CONNECTED)) return;
        throw;
    }
    if (count) throw TransportError("service-frame-invalid", "Unexpected input after the local reply.");
}
struct PipeListener::Impl {
    PrivateSecurity security;
    std::unique_ptr<SystemSecurity> system;
    std::wstring name;
    Handle pipe;
    std::unique_ptr<Operation> connection;
    explicit Impl(const std::string& key, bool broker = false) : name(pipe_name(key, broker)) {
        if (broker) {
            HANDLE raw = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) failed("Cannot identify broker host");
            Handle token(raw); const auto bytes = token_user(token.get());
            require(IsWellKnownSid(reinterpret_cast<const TOKEN_USER*>(bytes.data())->User.Sid, WinLocalSystemSid), "Only SYSTEM may own a broker listener");
            system = std::make_unique<SystemSecurity>();
        }
        pipe = make_pipe(true);
    }
    Handle make_pipe(bool first) { return system ? server_pipe(name, first, *system) : server_pipe(name, first, security); }
};
PipeListener::PipeListener(const std::string& endpoint) : impl_(std::make_unique<Impl>(endpoint)) {}
PipeListener::PipeListener(const std::string& endpoint, bool system) : impl_(std::make_unique<Impl>(endpoint, system)) {}
std::unique_ptr<PipeListener> PipeListener::system_broker(const std::string& endpoint) {
    return std::unique_ptr<PipeListener>(new PipeListener(endpoint, true));
}
PipeListener::~PipeListener() = default;
Channel PipeListener::accept(Deadline deadline, const Cancelled& cancelled) {
    for (;;) {
        active(deadline, cancelled);
        if (auto channel = try_accept()) return std::move(*channel);
        if (impl_->connection) wait_handle(impl_->connection->event.get(), deadline, cancelled);
    }
}
std::unique_ptr<Channel> PipeListener::try_accept() {
    const auto abandoned = [&](DWORD error) {
        if (error != ERROR_NO_DATA && error != ERROR_BROKEN_PIPE && error != ERROR_PIPE_NOT_CONNECTED) return false;
        impl_->connection.reset();
        DisconnectNamedPipe(impl_->pipe.get());
        return true;
    };
    try {
        bool connected = false;
        if (!impl_->connection) {
            impl_->connection = std::make_unique<Operation>(impl_->pipe.get());
            connected = ConnectNamedPipe(impl_->pipe.get(), &impl_->connection->value) != 0;
            if (!connected) {
                const auto error = GetLastError();
                if (abandoned(error)) return {};
                connected = error == ERROR_PIPE_CONNECTED;
                if (!connected && error != ERROR_IO_PENDING) failed("Cannot accept private pipe");
                impl_->connection->pending = !connected;
            }
        }
        if (!connected && impl_->connection->pending) {
            const auto state = WaitForSingleObject(impl_->connection->event.get(), 0);
            if (state == WAIT_TIMEOUT) return {};
            if (state != WAIT_OBJECT_0) failed("Cannot poll pipe acceptance");
            DWORD ignored = 0;
            if (!GetOverlappedResult(impl_->pipe.get(), &impl_->connection->value, &ignored, FALSE)) {
                const auto error = GetLastError();
                if (abandoned(error)) return {};
                throw std::system_error(static_cast<int>(error), std::system_category(), "Cannot finish pipe acceptance");
            }
            impl_->connection->pending = false;
        }
        impl_->connection.reset();
        std::string sid;
        if (impl_->system) sid = system_peer(impl_->pipe.get(), true);
        else peer(impl_->pipe.get(), true, impl_->security);
        auto next = impl_->make_pipe(false);
        auto accepted = std::move(impl_->pipe); impl_->pipe = std::move(next);
        auto state = std::make_unique<Channel::Impl>(std::move(accepted)); state->peer = std::move(sid);
        return std::unique_ptr<Channel>(new Channel(std::move(state)));
    } catch (...) { impl_->connection.reset(); DisconnectNamedPipe(impl_->pipe.get()); throw; }
}

struct Worker::Impl {
    ReadFile executable;
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    Handle process;
    DWORD pid = 0;
    std::unique_ptr<Channel> input, output;
    explicit Impl(const std::filesystem::path& path) : executable(path) {
        require(job.get() != nullptr, "Cannot create owned worker job");
        executable.require_executable_owner();
        require(executable.image_identity() == running_image_identity(), "Worker executable differs from the running kit");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) failed("Cannot protect worker job");
    }
};
Worker::Worker(const std::filesystem::path& executable, const std::string& role) : impl_(std::make_unique<Impl>(executable)) {
    require(role == "_service-worker" || role == "_refresh-worker" || role == "_resident-worker", "Unknown native worker role");
    const auto pair = [] {
        const auto key = random_key(); PipeListener listener(key);
        auto child = client_pipe(pipe_name(key), true);
        if (child.get() == INVALID_HANDLE_VALUE) failed("Cannot connect inherited worker channel");
        auto parent = listener.accept(Clock::now() + std::chrono::seconds(2));
        return std::pair{std::move(parent), std::move(child)};
    };
    auto [input, child_input] = pair(); auto [output, child_output] = pair();
    Handle error(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (error.get() == INVALID_HANDLE_VALUE) failed("Cannot open worker error sink");
    std::array<HANDLE, 3> inherited{child_input.get(), child_output.get(), error.get()};
    for (const auto handle : inherited) if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) failed("Cannot select worker stdio");
    SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    require(size > 0 && size < 65536, "Invalid process attribute size");
    std::vector<unsigned char> storage(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) failed("Cannot initialize worker handle allowlist");
    struct Attributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Attributes() { DeleteProcThreadAttributeList(value); } } cleanup{attributes};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), sizeof(inherited), nullptr, nullptr))
        failed("Cannot restrict inherited worker handles");
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_input.get(); startup.StartupInfo.hStdOutput = child_output.get(); startup.StartupInfo.hStdError = error.get();
    // The only argument is a fixed ASCII role; a validated executable path has no quotes.
    auto command = L"\"" + executable.native() + L"\" " + std::wstring(role.begin(), role.end());
    std::array<wchar_t, 32768> windows{};
    const auto length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    require(length && length < windows.size(), "Cannot locate Windows runtime");
    std::wstring environment = L"SystemRoot=" + std::wstring(windows.data(), length);
    const auto temporary = std::filesystem::temp_directory_path().native();
    environment += L'\0'; environment += L"TEMP=" + temporary;
    environment += L'\0'; environment += L"TMP=" + temporary;
    environment += L'\0'; environment += L'\0';
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
        environment.data(), executable.parent_path().c_str(), &startup.StartupInfo, &child)) failed("Cannot start native worker");
    impl_->process = Handle(child.hProcess); Handle thread(child.hThread); impl_->pid = child.dwProcessId;
    if (!AssignProcessToJobObject(impl_->job.get(), impl_->process.get()) || ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        TerminateProcess(impl_->process.get(), 1); WaitForSingleObject(impl_->process.get(), 5000);
        failed("Cannot bind native worker lifetime");
    }
    impl_->input = std::make_unique<Channel>(std::move(input));
    impl_->output = std::make_unique<Channel>(std::move(output));
}
Worker::~Worker() { stop(); }
Channel& Worker::input() { return *impl_->input; }
Channel& Worker::output() { return *impl_->output; }
unsigned long Worker::process_id() const { return impl_->pid; }
std::optional<std::uint64_t> Worker::rss_bytes() const {
    PROCESS_MEMORY_COUNTERS usage{};
    if (WaitForSingleObject(impl_->process.get(), 0) != WAIT_TIMEOUT ||
        !GetProcessMemoryInfo(impl_->process.get(), &usage, static_cast<DWORD>(sizeof(usage)))) return {};
    return static_cast<std::uint64_t>(usage.WorkingSetSize);
}
void Worker::stop() noexcept {
    if (impl_ && impl_->process.get() != INVALID_HANDLE_VALUE && impl_->process.get()) {
        TerminateJobObject(impl_->job.get(), 1);
        WaitForSingleObject(impl_->process.get(), 5000);
    }
}
unsigned long Worker::wait(Deadline deadline, const Cancelled& cancelled) {
    try { wait_handle(impl_->process.get(), deadline, cancelled); }
    catch (...) { stop(); throw; }
    DWORD code = 1;
    if (!GetExitCodeProcess(impl_->process.get(), &code)) failed("Cannot inspect owned worker exit");
    return code;
}
}
