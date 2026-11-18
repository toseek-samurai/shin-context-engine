#pragma once
#include "shin/observer/observer.h"
#include "shin/observer/local_package.h"
#include "context_types.h"
#include "shin/observer/local_registry.h"

namespace shin::observer {
inline bool present(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory)
        throw Error("path-unavailable", "Cannot inspect a local project path.");
    return fs::exists(status);
}

inline void guard_unbound_workspace(const fs::path& workspace, const std::optional<Json>& binding) {
    if (binding) return;
    if (present(workspace / "state.pending"))
        throw Error("binding-uncommitted", "Uncommitted initial binding requires review; it will not be replaced.");
    for (const auto& entry : fs::directory_iterator(workspace))
        if (entry.path().filename() != "observer.lock")
            throw Error("unbound-project-state", "Private project data exists without its binding; preserve it for review, not init or attach.");
}

inline void guard_ancestor_bindings(const fs::path& root, const fs::path& home) {
    unsigned depth = 0;
    for (auto current = root.parent_path(); current != current.root_path(); current = current.parent_path()) {
        if (++depth > 128) throw Error("root-too-deep", "Project ancestry exceeds the local admission limit.");
        // Even pending or damaged bindings own their directory until reviewed.
        if (present(home / "projects" / digest(current.string())))
            throw Error("ancestor-project-binding", "A parent project already has CLI state; select its root explicitly: " + current.string());
    }
}

inline std::string automatic_source_kit() {
    return std::string(SHIN_OBSERVER_VERSION) + "/" + SHIN_VERSION + "/" + shin::context::kGeneratorVersion;
}

// Compatibility diagnostics may still open an explicitly selected external route.
// Enrolled source roots use only the package's private publication.
inline bool packaged_managed_routes() {
    static const bool packaged = [] {
        const auto bin = current_executable_path().parent_path();
        return homebrew_managed() || bin.filename() == "bin" || present(bin.parent_path() / "shin-kit.json");
    }();
    return packaged;
}

inline bool current_automatic_primary(const Json& binding, const std::optional<fs::path>& bundled) {
    return binding.value("source_selection", "") == "automatic-project-files" &&
        binding.value("source_kit", "") == automatic_source_kit() &&
        (!packaged_managed_routes() || !bundled || binding.value("profile_dir", "") == bundled->string());
}

inline bool enrolled_source_root(const fs::path& root, const fs::path& home, const fs::path& workspace) {
    if (!present(home / "registry")) return false;
    for (const auto& entry : registered_local_projects(home)) {
        if (entry.root != root) continue;
        if (directory_identity(root) != entry.maintenance_root_identity ||
            directory_identity(workspace, true) != entry.maintenance_workspace_identity)
            throw Error("registry-binding-changed", "Enrolled source or workspace identity changed.");
        return true;
    }
    return false;
}
} // namespace shin::observer
