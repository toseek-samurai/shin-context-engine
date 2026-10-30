#pragma once

#include "context_types.h"
#include <algorithm>
#include <array>
#include <string_view>

namespace shin::observer {

// These identities authorize rebuilding from owned source, never reading an
// older index. Retire the previous identity here whenever the generator changes.
inline constexpr std::array kSourceRebuildGenerators{
    std::string_view{"shin-generator-v21-cpp-syntax"},
    std::string_view{"shin-generator-v22-compact-lexical"},
    std::string_view{"shin-generator-v23-unicode-lexical"},
};

inline bool retired_source_generator(std::string_view generator) {
    return std::find(kSourceRebuildGenerators.begin(), kSourceRebuildGenerators.end(), generator) !=
        kSourceRebuildGenerators.end();
}

inline bool rebuildable_source_generator(std::string_view generator) {
    return generator == shin::context::kGeneratorVersion || retired_source_generator(generator);
}

} // namespace shin::observer
