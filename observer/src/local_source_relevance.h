#pragma once
#include "context_internal.h"

namespace shin::observer {
inline bool local_path_below(const fs::path& path, const fs::path& parent) {
    return std::mismatch(path.begin(), path.end(), parent.begin(), parent.end()).second == parent.end();
}

// Shared by resident reads and background checkpoints. Events can over-invalidate,
// but an eligible new input must never be filtered out.
inline bool local_source_relevant(const shin::context::RuntimeOptions& options, const fs::path& path, bool directory) {
    namespace core = shin::context;
    if (!local_path_below(path, options.root)) return false;
    const auto relative = path.lexically_relative(options.root).generic_string();
    if (relative == ".") return directory;
    if (local_path_below(path, options.out_root / core::kGeneratedDir) || local_path_below(path, options.root / core::kGeneratedDir) ||
        core::shin_internal_control_path(relative)) return false;
    const auto& profile = options.profile;
    for (const auto* files : {&profile.root_files, &profile.optional_root_files})
        for (const auto& file : *files)
            if (relative == file || (directory && local_path_below(options.root / file, path))) return true;
    if (local_path_below(path, options.root / "docs")) {
        if (directory) return true;
        if (core::document_path_allowed(profile, relative) && core::extension_allowed(profile.source_extensions, relative)) return true;
    }
    for (const auto& source : profile.source_fact_roots) {
        auto base = (options.root / source).lexically_normal();
        if (base.filename().empty()) base = base.parent_path();
        if (directory && local_path_below(base, path)) return true;
        if (!local_path_below(path, base)) continue;
        const auto inside = path.lexically_relative(base);
        bool pruned = false;
        for (auto it = inside.begin(); it != inside.end(); ++it) {
            if (!directory && std::next(it) == inside.end()) break;
            if (core::should_skip_source_fact_dir(it->string())) { pruned = true; break; }
        }
        if (pruned) continue;
        if (directory || core::extension_allowed(profile.source_fact_extensions, relative)) return true;
        if (std::find(profile.source_fact_extractors.begin(), profile.source_fact_extractors.end(), "retool") !=
            profile.source_fact_extractors.end()) return true;
    }
    return false;
}
}
