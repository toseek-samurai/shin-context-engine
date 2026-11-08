#pragma once

#include "context_types.h"
#include "shin/observer/io.h"
#include <algorithm>
#include <regex>
#include <string_view>

namespace shin::observer {

inline bool source_engine_identity(const std::string& version) {
    static const std::regex format(R"(^[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}(?:-(dev|rc)\.[0-9]{1,9})?$)");
    return std::regex_match(version, format);
}

// This checks an opaque metadata identity, never compatibility with its data.
// Every kit rebuilds owned indexes from source; no historical decoder list.
inline bool rebuildable_source_generator(std::string_view generator) {
    return generator.starts_with("shin-generator-") && generator.size() > std::string_view("shin-generator-").size() &&
        generator.size() <= 128 &&
        std::all_of(generator.begin(), generator.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        });
}

inline bool superseded_source_generator(std::string_view generator) {
    return rebuildable_source_generator(generator) && generator != shin::context::kGeneratorVersion;
}

inline Json index_generation_policy(const fs::path& workspace, const std::string& snapshot) {
    return {{"schema_version", "shin.index-generation.v1"}, {"kit_version", SHIN_OBSERVER_VERSION},
        {"core_version", SHIN_VERSION}, {"generator_version", shin::context::kGeneratorVersion},
        {"workspace_identity", directory_identity(workspace, true)}, {"snapshot_id", snapshot}};
}

inline bool current_index_generation(const fs::path& workspace, const std::optional<std::string>& snapshot = std::nullopt) {
    const auto directory = workspace / "index-generation";
    std::error_code error;
    const auto status = fs::symlink_status(directory, error);
    if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status))) return false;
    if (error) throw Error("index-generation-unavailable", "Cannot inspect the derived generation policy.");
    try {
        StateStore store(directory, false);
        const auto saved = store.load();
        if (!saved || !saved->is_object() || !saved->contains("snapshot_id") || !saved->at("snapshot_id").is_string()) return false;
        const auto recorded = saved->at("snapshot_id").get<std::string>();
        if (recorded.size() != 24 || !recorded.starts_with("fnv1a64:") ||
            recorded.find_first_not_of("0123456789abcdef", 8) != std::string::npos) return false;
        // Refresh can reconcile an interrupted prepared-snapshot receipt
        // within this kit. Readers still require the exact published snapshot.
        return *saved == index_generation_policy(workspace, snapshot.value_or(recorded));
    } catch (const Error& failure) {
        if (failure.code != "invalid-json" && failure.code != "json-too-large" && failure.code != "state-too-large") throw;
        return false;
    }
}

inline void remember_index_generation(const fs::path& workspace, const std::string& snapshot) {
    const auto directory = workspace / "index-generation";
    ensure_private_directory(directory);
    StateStore store(directory, true);
    store.recover_pending();
    store.save(index_generation_policy(workspace, snapshot));
}

} // namespace shin::observer
