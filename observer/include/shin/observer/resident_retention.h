#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace shin::observer {
// Idle retention target, not a cap on active queries or preparation peaks.
inline constexpr std::uint64_t kResidentRetentionBytes = 768ULL * 1024 * 1024;
struct ResidentRetentionSample {
    std::optional<std::uint64_t> rss;
    bool idle;
    std::uint64_t idle_ms;
};
inline std::optional<std::size_t> resident_memory_victim(
    const std::vector<ResidentRetentionSample>& samples, std::uint64_t budget = kResidentRetentionBytes) {
    std::uint64_t total = 0;
    std::optional<std::size_t> victim;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& item = samples[i];
        if (!item.rss) return {}; // Unknown is not zero and cannot justify eviction.
        total += std::min(*item.rss, std::numeric_limits<std::uint64_t>::max() - total);
        if (item.idle && item.idle_ms >= 2000 && (!victim || item.idle_ms > samples[*victim].idle_ms)) victim = i;
    }
    return total > budget ? victim : std::nullopt;
}
}
