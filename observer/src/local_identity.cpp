#include "shin/observer/local_identity.h"
#include "shin/observer/local_registry.h"
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/rand.h>
#endif
#include <array>
#include <chrono>
#include <set>
#include <thread>

namespace shin::observer {
namespace {
bool present(const fs::path& path) { return fs::exists(fs::symlink_status(path)); }
[[noreturn]] void invalid() {
    throw Error("local-identity-conflict", "Local identity does not match this installation and working copy; no automatic replacement.");
}
bool uuid(const Json& value) {
    if (!value.is_string()) return false;
    const auto text = value.get<std::string>();
    if (text.size() != 36 || text[14] != '4' || std::string_view("89ab").find(text[19]) == std::string_view::npos) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (text[i] != '-') return false; }
        else if (std::string_view("0123456789abcdef").find(text[i]) == std::string_view::npos) return false;
    }
    return true;
}
std::string new_uuid() {
    std::array<unsigned char,16> bytes{};
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
#else
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
#endif
        throw Error("local-identity-unavailable", "Cannot generate a local installation identity.");
    bytes[6] = static_cast<unsigned char>((bytes[6] & 15U) | 64U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 63U) | 128U);
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
        result += "0123456789abcdef"[bytes[i] >> 4U];
        result += "0123456789abcdef"[bytes[i] & 15U];
    }
    return result;
}
void fields(const Json& value, const std::set<std::string>& expected) {
    if (!value.is_object() || value.size() != expected.size()) invalid();
    for (const auto& key : expected)
        if (!value.contains(key) || !value.at(key).is_string() || value.at(key).get<std::string>().empty()) invalid();
}
std::unique_ptr<StateStore> writer(const fs::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        try { return std::make_unique<StateStore>(path, true); }
        catch (const Error& error) {
            if (error.code != "writer-busy" || std::chrono::steady_clock::now() >= deadline) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}
void no_orphan_workspaces(const fs::path& home) {
    if (!present(home / "projects")) return;
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(home / "projects")) {
        if (++count > 4096 || present(entry.path() / "identity"))
            throw Error("local-identity-unavailable", "Existing working-copy identity requires the original installation identity.");
    }
}
Json installation(const fs::path& home, bool create) {
    const auto directory = home / "identity";
    const bool existed = present(directory);
    if (!existed && !create) throw Error("local-identity-unavailable", "Run shin-ce init to prepare local identity.");
    // Initializing another project must not replace a lost shared installation ID.
    if (!existed) no_orphan_workspaces(home);
    if (!existed) ensure_private_directory(directory);
    StateStore store(directory, create);
    auto value = store.load();
    if (!value) {
        if (existed) throw Error("local-identity-unavailable", "Installation identity is missing or uncommitted; it will not be regenerated.");
        value = Json{{"schema_version","shin.local-installation-identity.v1"}, {"installation_id",new_uuid()},
            {"home_identity",directory_identity(home,true)}, {"identity_directory",directory_identity(directory,true)},
            {"workspaces",Json::object()}};
        store.save(*value);
    }
    if (!value->is_object() || value->size() != 5 || !value->contains("workspaces") ||
        !value->at("workspaces").is_object() || value->at("workspaces").size() > 1024) invalid();
    for (const auto* field : {"schema_version","installation_id","home_identity","identity_directory"})
        if (!value->contains(field) || !value->at(field).is_string()) invalid();
    const auto hash = [](const std::string& text) {
        return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
    };
    for (const auto& [key,pin] : value->at("workspaces").items())
        if (!hash(key) || !pin.is_string() || !hash(pin.get<std::string>())) invalid();
    if (value->at("schema_version") != "shin.local-installation-identity.v1" || !uuid(value->at("installation_id")) ||
        value->at("home_identity") != directory_identity(home,true) ||
        value->at("identity_directory") != directory_identity(directory,true)) invalid();
    return *value;
}
std::string binding_key(const Json& binding, const fs::path& root, const fs::path& workspace) {
    if (!binding.is_object() || binding.value("root",Json(nullptr)) != root.string() ||
        binding.value("root_identity",Json(nullptr)) != directory_identity(root) ||
        binding.value("workspace_identity",Json(nullptr)) != directory_identity(workspace,true)) invalid();
    const auto schema = binding.value("schema_version",Json(nullptr));
    if (schema != "shin.local-project-binding.v1" && schema != "shin.local-route-binding.v1") invalid();
    Json stable;
    for (const auto* field : {"schema_version","root","root_identity","workspace_identity"}) stable[field] = binding.at(field);
    if (schema == "shin.local-route-binding.v1") {
        for (const auto* field : {"observer_config","observer_binding_key","index_identity"}) {
            if (!binding.contains(field) || !binding.at(field).is_string() || binding.at(field).get<std::string>().empty()) invalid();
            stable[field] = binding.at(field);
        }
    } else stable["profile_dir"] = binding.at("profile_dir");
    // Quality/profile revisions and snapshots may evolve without becoming a new checkout.
    return digest(stable.dump());
}
Json working_copy(const Json& host, const std::string& key, const fs::path& workspace, bool create, bool allow_unpinned = false) {
    const auto directory = workspace / "identity";
    const bool existed = present(directory);
    const auto slot = workspace.filename().string();
    const bool pinned = host.at("workspaces").contains(slot);
    if (!existed && pinned) throw Error("local-identity-unavailable", "A registered working-copy identity is missing; it will not be replaced.");
    if (!existed && !create) throw Error("local-identity-unavailable", "Run shin-ce init to prepare working-copy identity.");
    if (!existed) ensure_private_directory(directory);
    StateStore store(directory, create);
    auto value = store.load();
    if (!value) {
        if (existed) throw Error("local-identity-unavailable", "Working-copy identity is missing or uncommitted; it will not be regenerated.");
        value = Json{{"schema_version","shin.local-workspace-identity.v1"}, {"installation_id",host.at("installation_id")},
            {"local_workspace_id",new_uuid()}, {"binding_key",key}, {"identity_directory",directory_identity(directory,true)}};
        store.save(*value);
    }
    fields(*value, {"schema_version","installation_id","local_workspace_id","binding_key","identity_directory"});
    if (value->at("schema_version") != "shin.local-workspace-identity.v1" || !uuid(value->at("local_workspace_id")) ||
        value->at("installation_id") != host.at("installation_id") || value->at("binding_key") != key ||
        value->at("identity_directory") != directory_identity(directory,true)) invalid();
    if (pinned && host.at("workspaces").at(slot) != digest(value->dump())) invalid();
    if (!pinned && !create && !allow_unpinned) throw Error("local-identity-unavailable", "Working-copy identity enrollment is incomplete; retry shin-ce init.");
    return *value;
}
}

void check_local_identity(const fs::path& home, const fs::path& root, bool installation_only) {
    const auto workspace = home / "projects" / digest(root.string());
    const bool has_host = present(home / "identity"), has_workspace = present(workspace / "identity");
    // Init-only preflight waits for another installation enrollment to commit.
    auto guard = has_host ? writer(home) : nullptr;
    if (!has_host) no_orphan_workspaces(home);
    if (!has_host && !has_workspace) return; // Existing offline binding, no online identity adoption implied.
    const auto host = installation(home,false);
    if (installation_only) return;
    if (has_workspace || host.at("workspaces").contains(workspace.filename().string())) {
        StateStore binding(workspace,false);
        const auto saved = binding.load();
        if (!saved) invalid();
        // A valid stamp before the catalog commit is resumable only by init.
        (void)working_copy(host,binding_key(*saved,root,workspace),workspace,false,true);
    }
}

Json local_identity(const LocalProjectRequest& request, bool enroll) {
    const auto home = fs::absolute(request.home).lexically_normal();
    const auto root = fs::canonical(request.root);
    if (fs::canonical(home) != home) invalid();
    (void)directory_identity(home,true);
    const auto workspace = home / "projects" / digest(root.string());
    // Same binding lock and order as init/registry writes; no Server/network lock.
    auto owner = enroll ? writer(workspace) : nullptr;
    StateStore binding(workspace,false);
    const auto saved = binding.load();
    if (!saved || saved->value("prepared_snapshot",Json("")) == "")
        throw Error("local-identity-unavailable", "A prepared local project binding is required.");
    require_local_project_active(workspace);
    const auto key = binding_key(*saved,root,workspace);
    // A lost installation cannot replace a known working copy's installation ID.
    if (present(workspace / "identity") && !present(home / "identity"))
        throw Error("local-identity-unavailable", "The installation identity required by this working copy is missing.");
    // Short home-wide enrollment lock: one installation ID/catalog for concurrent roots.
    auto installation_guard = enroll ? writer(home) : nullptr;
    auto host = installation(home,enroll);
    const auto slot = workspace.filename().string();
    if (enroll && !host.at("workspaces").contains(slot) && host.at("workspaces").size() >= 1024)
        throw Error("local-identity-capacity", "The installation identity catalog is full; existing bindings are preserved.");
    const auto copy = working_copy(host,key,workspace,enroll);
    if (enroll && !host.at("workspaces").contains(slot)) {
        StateStore catalog(home / "identity",true);
        if (catalog.load() != host) invalid();
        host["workspaces"][slot] = digest(copy.dump());
        catalog.recover_pending();
        catalog.save(host);
    }
    const auto current = installation(home,false);
    for (const auto* field : {"installation_id","home_identity","identity_directory"})
        if (current.at(field) != host.at(field)) invalid();
    if (!current.at("workspaces").contains(slot) || current.at("workspaces").at(slot) != digest(copy.dump())) invalid();
    if (binding.load() != saved ||
        working_copy(host,binding_key(*saved,root,workspace),workspace,false) != copy) invalid();
    return {{"schema_version","shin.local-identity.v1"}, {"installation_id",host.at("installation_id")},
        {"local_workspace_id",copy.at("local_workspace_id")}, {"binding_key",key},
        {"server_binding",nullptr}, {"authentication",false}, {"content_currentness_assessed",false}};
}
} // namespace shin::observer
