#pragma once
#include "shin/observer/observer.h"
#include "context_internal.h"

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

inline void guard_existing_routes(const fs::path& root, const fs::path& home) {
    guard_ancestor_bindings(root, home);
    unsigned depth = 0;
    for (auto current = root; current != current.root_path(); current = current.parent_path()) {
        if (++depth > 128) throw Error("root-too-deep", "Project ancestry exceeds the local admission limit.");
        if (shin::context::find_existing_adoption_marker(current))
            throw Error("existing-project-adoption", "Existing Shin settings found. Connect them once with shin-ce init --observer-config FILE; the existing index will not be replaced.");
        if (present(current / "docs/kb/generated/manifest.json") || present(current / "docs/kb/.shin-context.lock"))
            throw Error("existing-project-publication", "Existing Shin index found. Connect it with shin-ce init --observer-config FILE; automatic replacement is disabled: " + current.string());
    }
}
} // namespace shin::observer
