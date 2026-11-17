#include "shin/observer/local_client_setup.h"
#include "shin/observer/local_package.h"
#include "shin/observer/observer.h"
#include <cctype>
#include <map>
#include <set>

namespace shin::observer {
namespace {
constexpr std::size_t maximum_files = 32, maximum_file = 64 * 1024, maximum_bundle = 128 * 1024;
bool present(const fs::path& path) { return fs::symlink_status(path).type() != fs::file_type::not_found; }
bool hash_value(const Json& value) {
    if (!value.is_string()) return false;
    const auto text = value.get<std::string>();
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool relative_file(const std::string& text) {
    const fs::path path(text);
    if (text.empty() || text.size() > 240 || path.is_absolute() || path.lexically_normal() != path ||
        text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./") != std::string::npos ||
        (path.extension() != ".md" && path.extension() != ".yaml")) return false;
    for (const auto& part : path) if (part.empty() || part == "." || part == "..") return false;
    return true;
}
void check_hashes(const Json& files) {
    if (!files.is_object() || files.size() > maximum_files)
        throw Error("agent-skill-invalid", "The instruction file inventory is invalid.");
    std::set<std::string> names;
    for (const auto& [name, hash] : files.items()) {
        auto folded = name;
        for (auto& ch : folded) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!relative_file(name) || !hash_value(hash) || !names.insert(folded).second)
            throw Error("agent-skill-invalid", "The instruction file inventory is unsafe.");
    }
}
using Contents = std::map<std::string, std::optional<std::string>>;
using Directories = std::map<fs::path, std::string>;
void observe_parents(fs::path path, Directories& directories) {
    while (!present(path)) path = path.parent_path();
    if (!directories.contains(path)) directories.emplace(path, directory_identity(path));
}
void check_directories(const Directories& directories) {
    for (const auto& [path, identity] : directories)
        if (directory_identity(path) != identity)
            throw Error("agent-skill-changed", "An instruction directory changed after review.");
}
void create_parents(const fs::path& path) {
    if (present(path)) { directory_identity(path); return; }
    create_parents(path.parent_path()); ensure_private_directory(path, true);
}
std::optional<std::string> contents(const fs::path& path) {
    return present(path) ? std::optional<std::string>(read_user_config(path, maximum_file)) : std::nullopt;
}
Json hash_of(const std::optional<std::string>& text) { return text ? Json(digest(*text)) : Json(); }
struct Bundle { Json hashes; Contents files; std::string version, revision; };
Bundle load_bundle(const fs::path& executable) {
    const auto bin = fs::canonical(executable).parent_path();
    for (const auto& prefix : {bin, package_payload_prefix(executable)}) {
        const auto root = prefix / "share/shin-observer/agent-skills/codex";
        if (!present(root)) continue;
        directory_identity(root);
        const auto descriptor = parse_json(read_text(root / "bundle.json", maximum_file, false));
        if (descriptor.value("schema_version", "") != "shin.codex-skill-bundle.v1" ||
            descriptor.value("name", "") != "shin-context" || !descriptor.contains("files") ||
            !descriptor.contains("version") || !descriptor.at("version").is_string() ||
            descriptor.at("version").get_ref<const std::string&>().size() > 64)
            throw Error("agent-skill-bundle-invalid", "The bundled Codex instructions are invalid.");
        Bundle bundle{descriptor.at("files"), {}, descriptor.at("version"), {}};
        check_hashes(bundle.hashes);
        if (!bundle.hashes.contains("SKILL.md")) throw Error("agent-skill-bundle-invalid", "The instruction entry is missing.");
        std::size_t total = 0;
        for (const auto& [name, checksum] : bundle.hashes.items()) {
            auto text = read_text(root / "shin-context" / fs::path(name), maximum_file, false);
            total += text.size();
            if (total > maximum_bundle || digest(text) != checksum.get<std::string>())
                throw Error("agent-skill-bundle-invalid", "The bundled instructions failed their byte check.");
            bundle.files.emplace(name, std::move(text));
        }
        bundle.revision = digest(bundle.hashes.dump());
        return bundle;
    }
    throw Error("agent-skill-bundle-unavailable", "This kit does not contain Codex instructions.");
}
struct Plan {
    fs::path target, state_dir;
    Bundle bundle;
    Json saved, pending_io, result;
    Contents current;
    Directories directories;
};
void validate_saved(const Json& saved, const fs::path& target) {
    if (saved == Json::object()) return;
    if (!saved.is_object() || saved.value("schema_version", "") != "shin.codex-skill-owner.v1" ||
        saved.value("directory", "") != target.string() || !saved.contains("files") ||
        saved.value("directory_identity", "") != directory_identity(target))
        throw Error("agent-skill-state-invalid", "The instruction ownership receipt is invalid.");
    check_hashes(saved.at("files"));
}
// Recovery never replays a write. It restores only bytes still equal to either
// side of our journal, and refuses to touch intervening user edits.
Json recovery_conflicts(const Json& saved, const fs::path& target, Contents* current = nullptr) {
    const auto& journal = saved.at("pending");
    if (!journal.is_object() || !journal.contains("edits") || !journal.at("edits").is_array() ||
        journal.at("edits").size() > maximum_files * 2 || !journal.contains("directories") ||
        !journal.at("directories").is_object() || journal.at("directories").size() > maximum_files * 2 + 1)
        throw Error("agent-skill-state-invalid", "The interrupted instruction update has an invalid journal.");
    Json conflicts = Json::array(); std::set<std::string> names;
    for (const auto& [relative, identity] : journal.at("directories").items()) {
        const fs::path suffix(relative);
        const auto directory = relative.empty() ? target : target / suffix;
        if ((!relative.empty() && (!relative_file(relative + "/placeholder.md") || suffix.is_absolute())) || !identity.is_string() || identity.get_ref<const std::string&>().size() > 512 ||
            directory_identity(directory) != identity.get<std::string>())
            throw Error("agent-skill-changed", "An interrupted instruction directory has changed.");
    }
    std::size_t total = 0;
    for (const auto& edit : journal.at("edits")) {
        if (!edit.is_object() || !edit.contains("path") || !edit.at("path").is_string() ||
            !edit.contains("before") || (!edit.at("before").is_null() && !edit.at("before").is_string()) ||
            !edit.contains("after") || (!edit.at("after").is_null() && !hash_value(edit.at("after"))))
            throw Error("agent-skill-state-invalid", "An interrupted instruction edit is invalid.");
        const auto name = edit.at("path").get<std::string>();
        if (!relative_file(name) || !names.insert(name).second)
            throw Error("agent-skill-state-invalid", "An interrupted instruction path is unsafe.");
        const auto before = edit.at("before");
        if (before.is_string()) {
            const auto size = before.get_ref<const std::string&>().size(); total += size;
            if (size > maximum_file || total > maximum_bundle) throw Error("agent-skill-state-invalid", "Instruction recovery exceeds its byte bound.");
        }
        const auto now = contents(target / name);
        if (current) current->emplace(name, now);
        if ((now ? Json(*now) : Json()) != before && hash_of(now) != edit.at("after")) conflicts.push_back(name);
    }
    return conflicts;
}
Plan preview(const fs::path& executable, const fs::path& config) {
    if (!config.is_absolute() || config.filename().empty() || config.lexically_normal() != config)
        throw Error("client-config-path", "Choose an absolute, normalized Codex settings path.");
    Plan plan; plan.target = config.parent_path() / "skills/shin-context";
    plan.state_dir = config.parent_path() / ".shin-agent-skills/shin-context";
    plan.bundle = load_bundle(executable); plan.saved = Json::object(); plan.pending_io = nullptr;
    observe_parents(config.parent_path(), plan.directories);
    observe_parents(plan.target, plan.directories);
    if (present(plan.state_dir)) {
        StateStore state(plan.state_dir, false); plan.saved = state.load().value_or(Json::object());
        observe_parents(plan.state_dir, plan.directories);
        if (present(plan.state_dir / "state.pending")) plan.pending_io = read_text(plan.state_dir / "state.pending", kMaximumStateBytes, true);
    }
    validate_saved(plan.saved, plan.target);
    const bool managed = plan.saved != Json::object();
    Json modified = Json::array(), changed = Json::array(), observed = Json::object();
    const bool recovery = plan.saved.contains("pending") || !plan.pending_io.is_null();
    if (plan.saved.contains("pending")) modified = recovery_conflicts(plan.saved, plan.target, &plan.current);
    else {
        std::set<std::string> names;
        for (const auto& [name, _] : plan.bundle.hashes.items()) names.insert(name);
        if (managed) for (const auto& [name, _] : plan.saved.at("files").items()) names.insert(name);
        std::size_t total = 0;
        for (const auto& name : names) {
            const auto path = plan.target / name;
            observe_parents(path.parent_path(), plan.directories);
            const auto now = contents(path); plan.current.emplace(name, now);
            if (now) total += now->size();
            if (total > maximum_bundle) throw Error("agent-skill-limit", "Installed instructions exceed the supported byte bound.");
            const auto actual = hash_of(now);
            const auto desired = plan.bundle.hashes.value(name, Json());
            const auto previous = managed ? plan.saved.at("files").value(name, Json()) : desired;
            // Missing newly introduced pages are safe; missing or edited owned
            // pages and collisions with unmanaged files need human review.
            if (actual != previous && (now || (managed && plan.saved.at("files").contains(name)))) modified.push_back(name);
            if (actual != desired) changed.push_back(name);
        }
    }
    for (const auto& [name, text] : plan.current) observed[name] = hash_of(text);
    Json directories = Json::object();
    for (const auto& [path, identity] : plan.directories) directories[path.string()] = identity;
    const auto revision = digest(Json::array({config.string(), plan.bundle.revision, plan.saved, plan.pending_io, observed, directories}).dump());
    const std::string status = recovery ? "recovery-required" : !modified.empty() ? "customized" :
        !changed.empty() ? (managed ? "update-available" : "not-installed") : "current";
    plan.result = {{"ok", true}, {"schema_version", "shin.codex-skill.v1"}, {"state", status},
        {"managed", managed}, {"can_apply", modified.empty()}, {"changed", !changed.empty()},
        {"changed_files", changed}, {"modified_files", modified}, {"directory", plan.target.string()},
        {"source_version", plan.bundle.version}, {"bundle_revision", plan.bundle.revision},
        {"revision", revision}, {"client_modified", false}};
    check_directories(plan.directories);
    return plan;
}
void replace_file(const fs::path& path, const std::optional<std::string>& before, const std::optional<std::string>& after) {
    if (contents(path) != before) throw Error("agent-skill-changed", "Instructions changed during update. User edits were preserved.");
    if (before == after) return;
    const auto identity = directory_identity(path.parent_path());
    if (!after) remove_user_archive(path, *before, maximum_file);
    else if (before) replace_user_config(path, *before, *after, identity);
    else if (!write_user_archive(path, *after)) throw Error("agent-skill-changed", "An instruction file appeared during update.");
}
void rollback(StateStore& state, const Json& saved, const fs::path& target) {
    if (!recovery_conflicts(saved, target).empty())
        throw Error("agent-skill-recovery-required", "An interrupted instruction update contains user edits. Preserve them and review the saved journal.");
    for (const auto& edit : saved.at("pending").at("edits")) {
        const auto path = target / edit.at("path").get<std::string>();
        const auto now = contents(path);
        const auto before = edit.at("before").is_null() ? std::nullopt : std::optional<std::string>(edit.at("before").get<std::string>());
        if (now != before && hash_of(now) != edit.at("after"))
            throw Error("agent-skill-recovery-required", "Instructions changed during recovery. The journal and user edits were preserved.");
        replace_file(path, now, before);
    }
    auto receipt = saved; receipt.erase("pending");
    if (receipt.value("was_unmanaged", false)) receipt = Json::object();
    else receipt.erase("was_unmanaged");
    state.save(receipt);
}
}

Json local_codex_skill(const fs::path& executable, const fs::path& config, const std::string& revision) {
    auto plan = preview(executable, config);
    if (revision.empty()) return plan.result;
    if (revision != plan.result.at("revision").get<std::string>()) throw Error("agent-skill-changed", "Instructions changed after review. Refresh before updating.");
    if (!plan.result.at("can_apply").get<bool>()) throw Error("agent-skill-customized", "User instruction edits need review; no files were overwritten.");
    check_directories(plan.directories);
    create_parents(plan.state_dir);
    StateStore state(plan.state_dir, true);
    if (state.load().value_or(Json::object()) != plan.saved ||
        (present(plan.state_dir / "state.pending") ? Json(read_text(plan.state_dir / "state.pending", kMaximumStateBytes, true)) : Json()) != plan.pending_io)
        throw Error("agent-skill-changed", "The instruction ownership receipt changed after review.");
    state.recover_pending();
    if (plan.saved.contains("pending")) {
        rollback(state, plan.saved, plan.target);
        auto result = preview(executable, config).result; result["recovered"] = true; result["client_modified"] = true;
        return result;
    }
    if (!plan.pending_io.is_null()) {
        auto result = preview(executable, config).result; result["recovered"] = true;
        return result;
    }
    // Capture every file parent after our own exclusive directory creation.
    Directories parents;
    for (const auto& [name, before] : plan.current) {
        create_parents((plan.target / name).parent_path());
        if (contents(plan.target / name) != before) throw Error("agent-skill-changed", "Instructions changed after review.");
        observe_parents((plan.target / name).parent_path(), parents);
    }
    check_directories(plan.directories);
    Json edits = Json::array(), parent_ids = Json::object();
    parent_ids[""] = directory_identity(plan.target);
    for (const auto& [path, identity] : parents) parent_ids[path.lexically_relative(plan.target).generic_string() == "." ? "" : path.lexically_relative(plan.target).generic_string()] = identity;
    // Publish the entry last. Unknown extra files are never enumerated or removed.
    for (const bool entry : {false, true}) for (const auto& [name, before] : plan.current) {
        if ((name == "SKILL.md") != entry) continue;
        const auto after = plan.bundle.files.contains(name) ? plan.bundle.files.at(name) : std::nullopt;
        if (before != after) edits.push_back({{"path", name}, {"before", before ? Json(*before) : Json()}, {"after", hash_of(after)}});
    }
    auto journal = plan.saved;
    if (journal == Json::object()) journal = {{"schema_version", "shin.codex-skill-owner.v1"}, {"directory", plan.target.string()}, {"directory_identity", directory_identity(plan.target)}, {"files", Json::object()}, {"was_unmanaged", true}};
    journal["pending"] = {{"edits", edits}, {"directories", parent_ids}};
    // StateStore bounds the entire recovery record before the first file write.
    if (!edits.empty()) state.save(journal);
    try {
        for (const auto& edit : edits) {
            check_directories(parents);
            const auto name = edit.at("path").get<std::string>();
            replace_file(plan.target / name, plan.current.at(name), plan.bundle.files.contains(name) ? plan.bundle.files.at(name) : std::nullopt);
        }
        check_directories(parents);
        for (const auto& [name, _] : plan.current) {
            const auto expected = plan.bundle.files.contains(name) ? plan.bundle.files.at(name) : std::nullopt;
            if (contents(plan.target / name) != expected) throw Error("agent-skill-changed", "Instructions changed before update confirmation.");
        }
        state.save({{"schema_version", "shin.codex-skill-owner.v1"}, {"directory", plan.target.string()},
            {"directory_identity", directory_identity(plan.target)},
            {"files", plan.bundle.hashes}, {"bundle_revision", plan.bundle.revision}, {"version", plan.bundle.version}});
    } catch (...) {
        if (!edits.empty()) {
            try { state.recover_pending(); rollback(state, journal, plan.target); }
            catch (...) { throw Error("agent-skill-recovery-required", "Instruction update could not be confirmed. Preserve the saved journal and review instructions before retrying."); }
        }
        throw;
    }
    plan.result["state"] = "current"; plan.result["managed"] = true;
    plan.result["changed"] = false; plan.result["changed_files"] = Json::array();
    plan.result["client_modified"] = !edits.empty(); plan.result["ownership_saved"] = true;
    return plan.result;
}
} // namespace shin::observer
