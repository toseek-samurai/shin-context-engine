#include "shin/observer/local_registry.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <thread>

namespace shin::observer {
namespace {
fs::path registry_home(const fs::path& value) {
    const auto home = fs::absolute(value).lexically_normal();
    if (fs::canonical(home) != home) throw Error("registry-home-unsafe", "Registry home must not contain symlinks.");
    (void)directory_identity(home, true);
    return home;
}

Json empty_registry(const fs::path& home) {
    return {{"schema_version", "shin.local-registry.v1"}, {"home_identity", directory_identity(home, true)},
            {"projects", Json::array()}};
}

void validate(const Json& value, const fs::path& home) {
    if (!value.is_object() || value.size() != 3 || !value.contains("schema_version") ||
        value.at("schema_version") != "shin.local-registry.v1" || !value.contains("home_identity") ||
        value.at("home_identity") != directory_identity(home, true) ||
        !value.contains("projects") || !value.at("projects").is_array() || value.at("projects").size() > kLocalServiceProjectLimit)
        throw Error("registry-invalid", "Private enrollment registry is invalid; preserve it for review.");
    std::set<fs::path> paths;
    for (const auto& item : value.at("projects")) {
        if (!item.is_object() || item.size() != 3) throw Error("registry-invalid", "Invalid project enrollment.");
        for (const auto* key : {"root", "root_identity", "workspace_identity"})
            if (!item.contains(key) || !item.at(key).is_string() || item.at(key).get<std::string>().empty() ||
                item.at(key).get<std::string>().size() > 4096 || item.at(key).get<std::string>().find('\0') != std::string::npos)
                throw Error("registry-invalid", "Invalid enrollment identity.");
        const fs::path path = item.at("root").get<std::string>();
        if (!path.is_absolute() || path.lexically_normal() != path || path == path.root_path())
            throw Error("registry-invalid", "Enrollment root must be absolute and normalized.");
        for (const auto& other : paths) {
            const auto [left, right] = std::mismatch(path.begin(), path.end(), other.begin(), other.end());
            if (left == path.end() || right == other.end())
                throw Error("registry-root-conflict", "Enrolled project roots overlap.");
        }
        paths.insert(path);
    }
}
} // namespace

LocalProjectLifecycle local_project_lifecycle(const fs::path& workspace) {
    const auto directory = workspace / "lifecycle";
    if (!fs::exists(fs::symlink_status(directory))) return {};
    StateStore store(directory, false);
    const auto value = store.load();
    // A writer may have created the private directory but not committed its
    // first intent yet. Fence the project until that intent is complete.
    if (!value) return {"detaching", 0};
    if (!value->is_object() || value->size() != 5 ||
        value->value("schema_version", Json(nullptr)) != "shin.local-lifecycle.v1" ||
        value->value("workspace_identity", Json(nullptr)) != directory_identity(workspace, true) ||
        !value->contains("root") || !value->at("root").is_string() ||
        digest(value->at("root").get<std::string>()) != workspace.filename() ||
        !value->contains("phase") || !value->at("phase").is_string() ||
        !value->contains("revision") || !value->at("revision").is_number_unsigned() || value->at("revision") == 0)
        throw Error("lifecycle-invalid", "Private project lifecycle is invalid; no automatic reactivation.");
    const auto phase = value->at("phase").get<std::string>();
    if (phase != "active" && phase != "detaching" && phase != "detached")
        throw Error("lifecycle-invalid", "Unknown private project lifecycle phase.");
    return {phase, value->at("revision").get<std::uint64_t>()};
}

LocalProjectLifecycle require_local_project_active(const fs::path& workspace) {
    const auto lifecycle = local_project_lifecycle(workspace);
    if (lifecycle.phase != "active")
        throw Error("project-detached", "This project is disconnected. Explicit shin-ce init is required to reconnect it.");
    return lifecycle;
}

void set_local_project_lifecycle(const fs::path& workspace, const Json& binding, const std::string& phase) {
    const auto previous = local_project_lifecycle(workspace);
    if (phase != "active" && phase != "detaching" && phase != "detached")
        throw Error("lifecycle-invalid", "Invalid private project lifecycle transition.");
    if (binding.at("workspace_identity") != directory_identity(workspace, true) ||
        digest(binding.at("root").get<std::string>()) != workspace.filename())
        throw Error("binding-identity-changed", "Lifecycle cannot replace a project's binding.");
    if (previous.revision == std::numeric_limits<std::uint64_t>::max())
        throw Error("lifecycle-invalid", "Lifecycle revision limit reached; reactivation is refused.");
    const auto directory = workspace / "lifecycle";
    ensure_private_directory(directory);
    StateStore store(directory, true);
    store.recover_pending();
    store.save({{"schema_version", "shin.local-lifecycle.v1"}, {"root", binding.at("root")},
        {"workspace_identity", binding.at("workspace_identity")}, {"phase", phase}, {"revision", previous.revision + 1}});
}

void unenroll_local_project(const fs::path& input_home, const Json& binding) {
    const auto home = registry_home(input_home);
    if (!fs::exists(fs::symlink_status(home / "registry"))) return;
    StateStore store(home / "registry", true);
    auto value = store.load();
    if (!value) throw Error("registry-invalid", "Project registry is missing its committed state.");
    validate(*value, home);
    auto& projects = value->at("projects");
    for (const auto& entry : projects) {
        if (entry.at("root") == binding.at("root") &&
            (entry.at("root_identity") != binding.at("root_identity") ||
             entry.at("workspace_identity") != binding.at("workspace_identity")))
            throw Error("registry-binding-changed", "Cannot detach another project identity.");
    }
    const auto size = projects.size();
    projects.erase(std::remove_if(projects.begin(), projects.end(), [&](const auto& entry) {
        return entry.at("root") == binding.at("root");
    }), projects.end());
    store.recover_pending();
    if (projects.size() != size) store.save(*value);
}

void ensure_local_registry(const fs::path& input_home) {
    const auto home = registry_home(input_home);
    const bool existed = fs::exists(fs::symlink_status(home / "registry"));
    ensure_private_directory(home / "registry");
    StateStore store(home / "registry", true);
    store.recover_pending();
    if (const auto value = store.load()) validate(*value, home);
    else {
        if (existed) throw Error("registry-unavailable", "Existing registry has no committed state; it was preserved.");
        store.save(empty_registry(home));
    }
}

std::vector<LocalProjectRequest> registered_local_projects(const fs::path& input_home) {
    const auto home = registry_home(input_home);
    StateStore store(home / "registry", false);
    const auto loaded = store.load();
    if (!loaded) throw Error("registry-unavailable", "No local project registry is available.");
    validate(*loaded, home);
    std::vector<LocalProjectRequest> result;
    for (const auto& item : loaded->at("projects")) {
        LocalProjectRequest request;
        request.root = item.at("root").get<std::string>();
        request.home = home;
        request.maintenance_root_identity = item.at("root_identity");
        request.maintenance_workspace_identity = item.at("workspace_identity");
        result.push_back(std::move(request));
    }
    return result;
}

void enroll_local_project(const LocalProjectRequest& input) {
    const auto home = registry_home(input.home);
    const auto root = fs::canonical(input.root);
    const auto workspace = home / "projects" / digest(root.string());
    const auto root_id = directory_identity(root);
    const auto workspace_id = directory_identity(workspace, true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::unique_ptr<StateStore> binding_store;
    while (!binding_store) {
        try { binding_store = std::make_unique<StateStore>(workspace, true, workspace_id, false); }
        catch (const Error& error) {
            if (error.code != "writer-busy" || std::chrono::steady_clock::now() >= deadline) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    const auto binding = binding_store->load();
    require_local_project_active(workspace);
    if (!binding || (binding->value("schema_version", "") != "shin.local-project-binding.v1" &&
                     binding->value("schema_version", "") != "shin.local-route-binding.v1") ||
        binding->value("root", "") != root.string() || binding->value("root_identity", "") != root_id ||
        binding->value("workspace_identity", "") != workspace_id || binding->value("prepared_snapshot", "").empty())
        throw Error("registry-binding-invalid", "Only a prepared local binding may enroll.");
    ensure_private_directory(home / "registry");
    for (;;) {
        try {
            StateStore store(home / "registry", true);
            auto value = store.load().value_or(empty_registry(home));
            validate(value, home);
            const Json entry{{"root", root.string()}, {"root_identity", root_id}, {"workspace_identity", workspace_id}};
            bool present = false;
            for (const auto& item : value.at("projects")) {
                if (item.at("root") != root.string()) continue;
                if (item != entry) throw Error("registry-binding-changed", "Enrollment identity changed; automatic replacement is forbidden.");
                present = true;
            }
            if (!present && value.at("projects").size() >= kLocalServiceProjectLimit)
                throw Error("registry-capacity", "The local service already has sixteen projects; existing enrollment is unchanged.");
            if (!present) value["projects"].push_back(entry);
            validate(value, home);
            if (directory_identity(root) != root_id || directory_identity(workspace, true) != workspace_id ||
                binding_store->load() != binding)
                throw Error("registry-binding-changed", "Project binding changed during enrollment; retry init.");
            store.recover_pending();
            if (!present) store.save(value);
            return;
        } catch (const Error& error) {
            if (error.code != "writer-busy" || std::chrono::steady_clock::now() >= deadline) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}
} // namespace shin::observer
