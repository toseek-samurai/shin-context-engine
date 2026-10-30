#pragma once
#include "io.h"
#include <string_view>

#ifndef SHIN_OBSERVER_HOMEBREW_PREFIX
#define SHIN_OBSERVER_HOMEBREW_PREFIX ""
#endif

namespace shin::observer {
inline bool homebrew_managed() { return std::string_view(SHIN_OBSERVER_HOMEBREW_PREFIX).size() != 0; }
inline fs::path package_payload_prefix(const fs::path& executable) {
    const auto file = fs::canonical(executable), parent = file.parent_path();
    if (parent.filename() == "MacOS" && parent.parent_path().filename() == "Contents" &&
        parent.parent_path().parent_path().extension() == ".app")
        return parent.parent_path().parent_path().parent_path();
    return parent.parent_path();
}

// Stable package paths are locators, never project/publication authority. The
// opt link must still select the exact payload running this operation.
inline fs::path stable_package_path(const fs::path& executable, const fs::path& prefix, const fs::path& relative) {
    if (!prefix.is_absolute() || prefix.lexically_normal() != prefix || relative.is_absolute() ||
        relative.empty() || relative.lexically_normal() != relative || *relative.begin() == "..")
        throw Error("package-prefix-invalid", "The managed package locator is invalid.");
    std::error_code error;
    const auto selected = fs::canonical(prefix, error);
    if (error || selected != package_payload_prefix(executable))
        throw Error("package-generation-changed", "Homebrew selected a different package generation. Rerun using the current brew command.");
    return prefix / relative;
}

inline fs::path package_binary(const fs::path& executable, const fs::path& name) {
    return homebrew_managed() ? stable_package_path(executable, SHIN_OBSERVER_HOMEBREW_PREFIX, fs::path("bin") / name) :
        fs::canonical(executable).parent_path() / name;
}

inline fs::path package_profile_registry(const fs::path& registry) {
    const auto stable = fs::path(SHIN_OBSERVER_HOMEBREW_PREFIX) / "share/shin-context/profiles";
    if (homebrew_managed() && fs::absolute(registry).lexically_normal() == stable) {
        const auto executable = current_executable_path();
        const auto path = stable_package_path(executable, SHIN_OBSERVER_HOMEBREW_PREFIX, "share/shin-context/profiles");
        if (!fs::is_directory(path) || fs::canonical(path) != package_payload_prefix(executable) / "share/shin-context/profiles")
            throw Error("package-registry-changed", "The bundled Homebrew profile registry is missing or redirected.");
        return path;
    }
    return fs::canonical(registry);
}
} // namespace shin::observer
