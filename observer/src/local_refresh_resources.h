#pragma once
#include <cstddef>
#include <cerrno>
#ifndef _WIN32
#include <dirent.h>
#include <sys/resource.h>
#endif

namespace shin::observer {
// Only the optional idle qualification cache yields to this reserve. It never
// changes OS limits, project membership, qualification or source-read authority.
// Keep room for a new native monitor and transient locks, inventory walks and
// parallel content reads before retaining another project's native handles.
inline constexpr std::size_t kRefreshActiveDescriptorReserve = 64;
inline bool local_refresh_descriptor_headroom(std::size_t reserve) {
#ifdef _WIN32
    (void)reserve;
    return true;
#else
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) return false;
    if (limit.rlim_cur == RLIM_INFINITY) return true;
    if (limit.rlim_cur <= reserve) return false;
#ifdef __APPLE__
    auto* descriptors = ::opendir("/dev/fd");
#else
    auto* descriptors = ::opendir("/proc/self/fd");
#endif
    if (!descriptors) return false;
    std::size_t used = 0;
    errno = 0;
    while (const auto* entry = ::readdir(descriptors)) {
        if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') ++used;
    }
    const bool complete = errno == 0;
    ::closedir(descriptors);
    // Includes the sampling directory itself: conservative by one descriptor.
    return complete && used <= limit.rlim_cur - reserve;
#endif
}
}
