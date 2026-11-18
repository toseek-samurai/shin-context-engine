#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#ifdef _WIN32
#include "shin/windows/file.h"
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace shin {
// Locate the running payload independently of cwd, PATH, argv[0] or project files.
inline std::filesystem::path running_program_path() {
#ifdef _WIN32
    return windows::running_image_path();
#else
    std::array<char, 4096> buffer{};
#ifdef __APPLE__
    std::uint32_t size = buffer.size();
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
#else
    const auto count = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size() - 1) return {};
#endif
    std::error_code error;
    auto path = std::filesystem::canonical(buffer.data(), error);
    return error ? std::filesystem::path{} : path;
#endif
}
} // namespace shin
