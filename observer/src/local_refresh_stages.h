#pragma once
#include <algorithm>
#include <iterator>
#include <string_view>

namespace shin::observer {
// Shared by worker progress, deadlines and retained diagnostics. Only these
// fixed identifiers may cross the diagnostic boundary; never paths or errors.
inline bool local_refresh_stage(std::string_view stage) {
    constexpr std::string_view stages[]{"admission", "observation", "candidate-build", "candidate-validation",
        "publication", "qualification", "source-cache", "candidate-source-cache", "source-cache-transfer",
        "final-verification", "qualification-cache", "qualification-snapshot", "qualification-generation",
        "qualification-profile", "qualification-doctor", "qualification-adoption", "qualification-fingerprint",
        "qualification-ready"};
    return std::find(std::begin(stages), std::end(stages), stage) != std::end(stages);
}
}
