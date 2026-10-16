#pragma once
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
namespace shin::observer {
std::optional<std::string> local_stdin_read();
bool local_stdout_write(std::string_view bytes, const std::atomic<bool>& running, std::chrono::steady_clock::time_point deadline);
}
