#include "shin/observer/local_stdio.h"
#include "shin/observer/io.h"
#include <algorithm>
#include <array>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace shin::observer {
using Clock = std::chrono::steady_clock;
std::optional<std::string> local_stdin_read() {
    std::array<char, 4096> bytes{};
#ifdef _WIN32
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    if (GetFileType(input) != FILE_TYPE_PIPE) throw Error("mcp-stdio-invalid", "MCP requires piped standard input.");
    DWORD available = 0;
    if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) return std::string{};
    if (!available) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); return {}; }
    DWORD count = 0;
    if (!ReadFile(input, bytes.data(), std::min<DWORD>(available, static_cast<DWORD>(bytes.size())), &count, nullptr)) return std::string{};
    return std::string(bytes.data(), count);
#else
    pollfd input{STDIN_FILENO, POLLIN, 0};
    const int ready = ::poll(&input, 1, 20);
    if (!ready || (ready < 0 && errno == EINTR)) return {};
    if (ready < 0 || !(input.revents & (POLLIN | POLLHUP))) return std::string{};
    const auto count = ::read(STDIN_FILENO, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR) return {};
    if (count <= 0) return std::string{};
    return std::string(bytes.data(), static_cast<std::size_t>(count));
#endif
}
bool local_stdout_write(std::string_view bytes, const std::atomic<bool>& running, Clock::time_point deadline) {
#ifdef _WIN32
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetFileType(output) != FILE_TYPE_PIPE) return false;
    const auto done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return false;
    std::atomic<bool> cancelled{false};
    bool complete = false;
    std::thread writer;
    try {
        writer = std::thread([&] {
            std::size_t sent = 0;
            while (running && !cancelled && sent < bytes.size()) {
                DWORD count = 0;
                if (!WriteFile(output, bytes.data() + sent, static_cast<DWORD>(bytes.size() - sent), &count, nullptr) || !count) break;
                sent += count;
            }
            complete = sent == bytes.size(); SetEvent(done);
        });
    } catch (...) { CloseHandle(done); throw; }
    while (WaitForSingleObject(done, 20) == WAIT_TIMEOUT) {
        if (!running || Clock::now() >= deadline) {
            cancelled = true;
            // Retry cancellation across the check-to-WriteFile race. Never free
            // its buffer while synchronous IO can still reference it.
            CancelSynchronousIo(writer.native_handle());
            if (Clock::now() >= deadline + std::chrono::seconds(2)) TerminateProcess(GetCurrentProcess(), 3);
        }
    }
    writer.join(); CloseHandle(done);
    return complete && !cancelled;
#else
    std::size_t sent = 0;
    while (running && sent < bytes.size() && Clock::now() < deadline) {
        pollfd output{STDOUT_FILENO, POLLOUT, 0};
        if (::poll(&output, 1, 20) <= 0) continue;
        if (!(output.revents & POLLOUT)) break;
        const auto count = ::write(STDOUT_FILENO, bytes.data() + sent, std::min<std::size_t>(512, bytes.size() - sent));
        if (count > 0) sent += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
    return sent == bytes.size();
#endif
}
}
