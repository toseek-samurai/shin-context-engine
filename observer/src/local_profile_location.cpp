#include "shin/observer/local_profile_location.h"
#include "shin/observer/local_publication.h"
#include "shin/observer/local_registry.h"
#include "shin/observer/local_service.h"
#include "context_internal.h"
#include "generated_io.h"
#include "snapshot_verification.h"
#include <map>
#include <set>

namespace shin::observer {
namespace {
namespace core = shin::context;
namespace generated = shin::generated_io;
void require(bool value, const char* message) {
    if (!value) throw Error("profile-relocation-conflict", message);
}
Json relocation_response(Json value) {
    value["schema_version"] = "shin.ce-response.v1";
    value["tool"] = "shin-ce";
    value["version"] = SHIN_OBSERVER_VERSION;
    value["core_version"] = SHIN_VERSION;
    value["delivery_performed"] = false;
    value["consumer_mutated"] = false;
    return value;
}
std::string route_key(Json binding) {
    for (const auto* key : {"prepared_snapshot", "core_version", "generator_version"}) binding.erase(key);
    return digest(binding.dump());
}
std::string revision_authority(Json binding, const std::string& token) {
    binding.erase("prepared_snapshot");
    return digest(token + binding.dump());
}
Json staged_json(const fs::path& directory, const std::string& name, const std::string& side,
                 const std::string& expected) {
    const auto text = read_text(directory / (name + "-" + side + ".json"), kMaximumStateBytes, true);
    require(digest(text) == expected, "Relocation evidence changed; preserve it for review.");
    return parse_json(text);
}
}

Json relocate_local_profile(const LocalProjectRequest& request) {
    require(request.profile_dir_explicit && request.profile_dir.has_value(), "Select an explicit destination profile directory.");
    const auto root = fs::canonical(request.root);
    const auto home = fs::absolute(request.home).lexically_normal();
    require(fs::canonical(home) == home, "Private home changed.");
    const auto destination = fs::absolute(*request.profile_dir).lexically_normal();
    require(fs::canonical(destination) == destination, "Destination must be an existing canonical profile directory.");
    const auto destination_identity = directory_identity(destination);
    const auto workspace = home / "projects" / digest(root.string());
    const auto root_identity = directory_identity(root), workspace_identity = directory_identity(workspace, true);
    auto control = lock_local_service_control(home);
    std::unique_ptr<StateStore> service;
    try { service = std::make_unique<StateStore>(home / "service", true); }
    catch (const Error& error) {
        if (error.code != "writer-busy") throw;
        throw Error("profile-relocation-requires-stopped-service", "Stop the local service before moving its profile registry.");
    }
    StateStore binding_store(workspace, true, workspace_identity);
    const auto enrolled = registered_local_projects(home);
    require(std::any_of(enrolled.begin(), enrolled.end(), [&](const auto& entry) {
        return entry.root == root && entry.maintenance_root_identity == root_identity &&
            entry.maintenance_workspace_identity == workspace_identity;
    }), "Project enrollment or directory identity changed.");
    const auto registry_text = read_text(home / "registry/state.json", kMaximumStateBytes, true);
    require(require_local_project_active(workspace).phase == "active", "Project is not active.");
    const auto transaction = workspace / "profile-location";
    ensure_private_directory(transaction);
    StateStore journal(transaction, true);
    journal.recover_pending();
    auto intent = journal.load();
    fs::path payload;
    if (intent) {
        require(intent->value("schema_version", "") == "shin.profile-location.v1" &&
            (intent->value("phase", "") == "prepared" || intent->value("phase", "") == "complete") &&
            intent->at("root") == root.string() && intent->at("root_identity") == root_identity &&
            intent->at("workspace_identity") == workspace_identity, "Unknown relocation journal requires review.");
        const auto name = intent->at("payload").get<std::string>();
        require(name.size() == 72 && name.starts_with("payload-") &&
            name.substr(8).find_first_not_of("0123456789abcdef") == std::string::npos,
            "Invalid relocation payload identity.");
        payload = transaction / name;
        require(intent->at("payload_identity") == directory_identity(payload, true), "Relocation payload directory changed.");
    }
    const auto current_binding = binding_store.load();
    require(current_binding.has_value(), "Project binding is missing.");
    require(current_binding->value("schema_version", "") == "shin.local-route-binding.v1",
        "Only an explicitly attached permanent profile can be relocated.");
    const bool resuming = intent && intent->value("phase", "") == "prepared";
    Json old_binding = *current_binding;
    if (resuming) {
        require(intent->value("schema_version", "") == "shin.profile-location.v1" &&
            intent->at("root") == root.string() && intent->at("workspace_identity") == workspace_identity &&
            intent->at("destination") == destination.string() && intent->at("destination_identity") == destination_identity,
            "Pending relocation belongs to a different request or identity.");
        const auto& row = intent->at("parts").back();
        require(row.at("name") == "binding", "Relocation binding evidence is missing.");
        old_binding = staged_json(payload, "binding", "before", row.at("before_sha256"));
    }
    require(old_binding.value("schema_version", "") == "shin.local-route-binding.v1" &&
        old_binding.at("root") == root.string() && old_binding.at("root_identity") == root_identity &&
        old_binding.at("workspace_identity") == workspace_identity, "Attached route identity changed.");
    const fs::path config_file = old_binding.at("observer_config").get<std::string>();
    const auto config_parent_identity = directory_identity(config_file.parent_path(), true);
    std::string old_config_text = read_text(config_file, kMaximumStateBytes, true);
    if (resuming) {
        old_config_text = read_text(payload / "config-before.json", kMaximumStateBytes, true);
        require(intent->at("config_before_sha256") == digest(old_config_text), "Original configuration evidence changed.");
    }
    const auto old_config = config_from_json(parse_json(old_config_text), config_file);
    require(old_config.root == root && !old_config.outbox_enabled, "Relocation requires the same local-only project route.");
    require(old_binding.at("observer_binding_key") == old_config.binding_key &&
        old_binding.at("index_identity") == directory_identity(old_config.out_dir, true), "Index or Observer binding changed.");
    std::unique_ptr<StateStore> watch;
    if (fs::exists(workspace / "watch")) watch = std::make_unique<StateStore>(workspace / "watch", true);
    std::map<std::string, std::unique_ptr<StateStore>> stores;
    stores["publication"] = std::make_unique<StateStore>(workspace / "publication", true);
    stores["observer"] = std::make_unique<StateStore>(old_config.state_dir, true, old_config.binding.at("state_identity"));
    for (const auto* name : {"marker-policy", "config-policy", "profile-revisions"})
        if (fs::exists(workspace / name)) stores[name] = std::make_unique<StateStore>(workspace / name, true);
    auto current_publication = stores.at("publication")->load();
    require(current_publication.has_value(), "Publication authority is missing.");
    Json old_publication = *current_publication;
    if (resuming) {
        const auto& rows = intent->at("parts");
        const auto row = std::find_if(rows.begin(), rows.end(), [](const auto& item) { return item.at("name") == "publication"; });
        require(row != rows.end(), "Publication evidence is missing.");
        old_publication = staged_json(payload, "publication", "before", row->at("before_sha256"));
    }
    const auto token = old_publication.at("token").get<std::string>();
    require(old_publication.at("phase") == "active" && old_publication.value("pending_snapshot", "").empty() &&
        old_publication.value("pending_profile_hash", "").empty(), "Finish any pending publication before relocating.");
    generated::IndexLock writer(old_config.out_dir, generated::LockMode::exclusive, generated::LockTarget::writer, token);
    require(generated::publisher_owner(old_config.out_dir) == std::optional<std::string>(token), "Publisher ownership changed.");
    const auto verified = core::load_verified_snapshot(old_config.out_dir, old_config.max_snapshot_bytes, old_config.max_file_bytes);
    require(old_publication.at("last_snapshot") == verified.snapshot_id, "Canonical snapshot changed.");
    generated::IndexLock reader_fence(old_config.out_dir, generated::LockMode::exclusive);
    const auto profile_name = old_config.profile + ".json";
    const auto destination_text = read_text(destination / profile_name, 1024 * 1024, false);
    const auto approved_hash = old_publication.value("approved_profile_hash", old_binding.at("profile_config_hash").get<std::string>());
    require(core::make_profile(old_config.profile, destination).config_hash == approved_hash &&
        verified.profile_config_hash == approved_hash, "Destination profile is not the approved snapshot policy.");
    if (!resuming) {
        require(!fs::exists(workspace / "state.pending"), "Unattributed pending binding requires review before relocation.");
        require(read_text(old_config.profile_dir / profile_name, 1024 * 1024, false) == destination_text,
            "Destination profile bytes differ from the existing policy.");
        LocalPublication authority(workspace, old_binding, old_config);
        authority.verify();
        require(authority.approved_profile_hash() == approved_hash, "Profile approval changed.");
        if (!old_binding.at("marker").get<std::string>().empty())
            require(digest(read_text(root / old_binding.at("marker").get<std::string>(), 1024 * 1024, false)) == authority.approved_marker_hash(),
                "Existing marker has an unapproved revision.");
        if (old_binding.at("observer_config_hash") != digest(old_config_text)) {
            require(stores.contains("config-policy"), "Existing configuration has an unapproved revision.");
            const auto policy = stores.at("config-policy")->load();
            require(policy && policy->at("route_key") == route_key(old_binding) &&
                policy->at("approved_config_hash") == digest(old_config_text), "Existing configuration has an unapproved revision.");
        }
        if (old_config.profile_dir == destination) {
            return relocation_response({{"ok", true}, {"command", "profile-relocate"}, {"changed", false}, {"index_mutated", false},
                {"profile_directory", destination.string()}, {"readiness_assessed", false}});
        }
        (void)observer_status(old_config);
        auto next_config_json = parse_json(old_config_text);
        next_config_json["profile_dir"] = destination.string();
        const auto next_config_text = next_config_json.dump(2) + "\n";
        const auto next_config = config_from_json(next_config_json, config_file);
        auto next_binding = old_binding;
        next_binding["observer_config_hash"] = digest(next_config_text);
        next_binding["observer_binding_key"] = next_config.binding_key;
        const auto payload_name = "payload-" + digest(old_binding.dump() + destination.string() + std::to_string(now_ms()));
        payload = transaction / payload_name;
        ensure_private_directory(payload, true);
        if (intent) write_private_archive(transaction / ("completed-" + intent->at("payload").get<std::string>() + ".json"), intent->dump());
        Json parts = Json::array();
        const auto stage = [&](const std::string& name, const Json& before, const Json& after) {
            const auto a = before.dump(), z = after.dump();
            write_private_archive(payload / (name + "-before.json"), a);
            write_private_archive(payload / (name + "-after.json"), z);
            parts.push_back({{"name", name}, {"before_sha256", digest(a)}, {"after_sha256", digest(z)}});
        };
        for (auto& [name, store] : stores) {
            require(!fs::exists((name == "observer" ? old_config.state_dir : workspace / name) / "state.pending"),
                "Unattributed pending metadata requires review before relocation.");
            const auto value = store->load();
            if (!value) continue;
            auto next = *value;
            if (name == "observer") {
                require(next.at("binding") == old_config.binding && next.at("binding_key") == old_config.binding_key,
                    "Observer state belongs to a different route.");
                next["binding"] = next_config.binding;
                next["binding_key"] = next_config.binding_key;
            } else if (name == "publication") {
                auto route = next_binding; route.erase("prepared_snapshot");
                route["output"] = old_config.out_dir.string(); route["home_identity"] = directory_identity(home, true);
                next["route"] = route;
            } else if (name == "profile-revisions") {
                require(next.at("authority") == revision_authority(old_binding, token), "Profile history belongs to a different authority.");
                next["authority"] = revision_authority(next_binding, token);
            } else {
                require(next.at("route_key") == route_key(old_binding), "Policy history belongs to a different route.");
                if (name == "config-policy") {
                    require(next.at("approved_config_hash") == digest(old_config_text), "Existing configuration is not approved.");
                    next["approved_config_hash"] = digest(next_config_text);
                }
                next["route_key"] = route_key(next_binding);
            }
            stage(name, *value, next);
        }
        stage("binding", old_binding, next_binding); // commit binding last
        write_private_archive(payload / "config-before.json", old_config_text);
        write_private_archive(payload / "config-after.json", next_config_text);
        intent = Json{{"schema_version", "shin.profile-location.v1"}, {"phase", "prepared"}, {"root", root.string()},
            {"root_identity", root_identity}, {"workspace_identity", workspace_identity},
            {"payload", payload_name}, {"payload_identity", directory_identity(payload, true)},
            {"destination", destination.string()}, {"destination_identity", destination_identity},
            {"profile_sha256", digest(destination_text)}, {"config_file", config_file.string()},
            {"config_parent_identity", config_parent_identity}, {"config_before_sha256", digest(old_config_text)},
            {"config_after_sha256", digest(next_config_text)}, {"registry_sha256", digest(registry_text)},
            {"snapshot_id", verified.snapshot_id}, {"manifest_sha256", digest(verified.manifest_content)}, {"parts", parts}};
        journal.save(*intent);
    }
    require(intent->at("root_identity") == root_identity && intent->at("registry_sha256") == digest(registry_text) &&
        intent->at("profile_sha256") == digest(destination_text) && intent->at("config_file") == config_file.string() &&
        intent->at("config_parent_identity") == config_parent_identity && intent->at("snapshot_id") == verified.snapshot_id &&
        intent->at("manifest_sha256") == digest(verified.manifest_content), "Relocation inputs changed; preserve the journal for review.");
    const auto next_config_text = read_text(payload / "config-after.json", kMaximumStateBytes, true);
    require(intent->at("config_after_sha256") == digest(next_config_text), "Prepared configuration changed.");
    const auto held_config = read_text(config_file, kMaximumStateBytes, true);
    require(held_config == old_config_text || held_config == next_config_text, "Configuration changed outside this relocation.");
    std::set<std::string> expected_parts{"binding"}, actual_parts;
    for (const auto& [name, store] : stores) if (store->load()) expected_parts.insert(name);
    require(intent->at("parts").is_array(), "Invalid relocation metadata list.");
    for (const auto& part : intent->at("parts"))
        require(actual_parts.insert(part.at("name").get<std::string>()).second, "Duplicate relocation metadata store.");
    require(expected_parts == actual_parts && intent->at("parts").back().at("name") == "binding", "Relocation metadata list changed.");
    for (const auto& part : intent->at("parts")) {
        const auto name = part.at("name").get<std::string>();
        require(name == "binding" || stores.contains(name), "Relocation contains an unknown metadata store.");
        const auto before = staged_json(payload, name, "before", part.at("before_sha256"));
        const auto after = staged_json(payload, name, "after", part.at("after_sha256"));
        auto& store = name == "binding" ? binding_store : *stores.at(name);
        // An interrupted StateStore save may have left exactly this next value.
        const auto directory = name == "binding" ? workspace : name == "observer" ? old_config.state_dir : workspace / name;
        if (fs::exists(directory / "state.pending")) {
            require(parse_json(read_text(directory / "state.pending", kMaximumStateBytes, true)) == after,
                "Pending state is not attributable to this relocation.");
            store.recover_pending();
        }
        const auto current = store.load();
        require(current && (*current == before || *current == after), "Metadata changed outside this relocation.");
        if (name == "binding" && held_config != next_config_text)
            replace_user_config(config_file, old_config_text, next_config_text, config_parent_identity);
        if (*current != after) store.save(after);
        require(store.load() == std::optional<Json>(after), "Metadata write did not confirm completion.");
    }
    const auto next_config = config_from_json(parse_json(next_config_text), config_file);
    LocalPublication authority(workspace, *binding_store.load(), next_config);
    authority.verify();
    (void)observer_status(next_config);
    require(directory_identity(destination) == destination_identity &&
        directory_identity(root) == root_identity && directory_identity(workspace, true) == workspace_identity &&
        old_binding.at("index_identity") == directory_identity(old_config.out_dir, true) &&
        old_config.binding.at("state_identity") == directory_identity(old_config.state_dir, true) &&
        read_text(destination / profile_name, 1024 * 1024, false) == destination_text &&
        read_text(home / "registry/state.json", kMaximumStateBytes, true) == registry_text &&
        read_text(old_config.out_dir / "docs/kb/generated/manifest.json", old_config.max_file_bytes, false) == verified.manifest_content,
        "Relocation authority or snapshot changed before completion.");
    (*intent)["phase"] = "complete";
    journal.save(*intent);
    return relocation_response({{"ok", true}, {"command", "profile-relocate"}, {"changed", true}, {"root", root.string()},
        {"profile_directory", destination.string()}, {"snapshot_id", verified.snapshot_id},
        {"index_mutated", false}, {"history_preserved", true}, {"readiness_assessed", false},
        {"next_action", "start-service-and-wait-for-qualification"}});
}
}
