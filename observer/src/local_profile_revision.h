#pragma once
#include "shin/observer/local_publication.h"
#include "context_internal.h"
#include "profile_policy.h"

namespace shin::observer {
// This cache is evidence of an already approved revision, never a grant itself.
// Keep the approved and proposed bytes together so an interrupted approval can
// resume without guessing the previous policy from the current source file.
class LocalProfileRevision {
public:
    LocalProfileRevision(const fs::path& workspace, const Json& binding, const LocalPublication& grant)
        : directory_(workspace / "profile-revisions") {
        auto identity = binding;
        identity.erase("prepared_snapshot");
        authority_ = digest(grant.token() + identity.dump());
    }
    std::optional<std::string> approved(const std::string& hash) const {
        const auto record = load();
        if (!record || !record->at("profiles").contains(hash)) return {};
        return record->at("profiles").at(hash).get<std::string>();
    }
    void remember(const std::string& approved_hash, const std::string& text) {
        if (text.size() > 96 * 1024) throw Error("profile-baseline-budget", "Profile revision exceeds automatic review storage limits.");
        auto previous = approved(approved_hash);
        Json profiles = Json::object();
        if (previous) profiles[approved_hash] = *previous;
        profiles[shin::context::fnv1a64(text)] = text;
        ensure_private_directory(directory_);
        StateStore store(directory_, true);
        store.recover_pending();
        store.save({{"schema_version", "shin.profile-revisions.v1"}, {"authority", authority_}, {"profiles", profiles}});
    }
    Json rejection(const std::string& hash, const std::string& worktree) const {
        const auto value = load();
        if (!value || !value->contains("rejected")) return nullptr;
        const auto& rejected = value->at("rejected");
        return rejected.value("profile", "") == hash && (worktree.empty() || rejected.value("worktree", "") == worktree) ?
            rejected.at("diagnostic") : Json();
    }
    void reject(const std::string& hash, const std::string& worktree, const Json& diagnostic) {
        auto value = load();
        if (!value) throw Error("profile-baseline-invalid", "Approved profile evidence disappeared.");
        (*value)["rejected"] = {{"profile", hash}, {"worktree", worktree},
            {"diagnostic", bounded_validation_diagnostic(diagnostic)}};
        StateStore store(directory_, true);
        store.recover_pending(); store.save(*value);
    }
private:
    std::optional<Json> load() const {
        if (!fs::exists(fs::symlink_status(directory_))) return {};
        StateStore store(directory_, false);
        const auto value = store.load();
        if (!value) return {};
        if (value->value("schema_version", "") == "shin.profile-revisions.v1" &&
            value->value("authority", "") != authority_) return {};
        if (value->value("schema_version", "") != "shin.profile-revisions.v1" || !value->contains("profiles") ||
            !value->at("profiles").is_object() || value->at("profiles").size() > 2)
            throw Error("profile-baseline-invalid", "Approved profile evidence is invalid; preserve it for review.");
        for (const auto& [hash, text] : value->at("profiles").items()) {
            if (!text.is_string() || text.get_ref<const std::string&>().size() > 96 * 1024 ||
                shin::context::fnv1a64(text.get_ref<const std::string&>()) != hash)
                throw Error("profile-baseline-invalid", "Approved profile evidence does not match its revision.");
        }
        return value;
    }
    fs::path directory_;
    std::string authority_;
};

inline constexpr const char* membership_fields[] = {
    "rootFiles", "root_files", "optionalRootFiles", "optional_root_files", "sourceFactRoots", "source_fact_roots"
};

inline bool safe_profile_membership_change(const fs::path& root, const std::string& before, const std::string& after) {
    auto old = shin::context::parse_json_text(before, "approved profile");
    auto next = shin::context::parse_json_text(after, "candidate profile");
    if (!old.is_object() || !next.is_object()) return false;
    for (const auto* field : membership_fields) {
        if (next.contains(field)) {
            if (!next.at(field).is_array()) return false;
            for (const auto& value : next.at(field)) {
                if (!value.is_string()) return false;
                const auto path = value.get<std::string>();
                const bool project_root = path == "." && (std::string_view(field) == "sourceFactRoots" ||
                                                            std::string_view(field) == "source_fact_roots");
                if ((!project_root && !shin::context::profile_relative_path_valid(path)) ||
                    fs::path(path).lexically_normal().generic_string() != path ||
                    shin::context::profile_path_contains_symlink(root, path)) return false;
            }
        }
        old.erase(field); next.erase(field);
    }
    return old == next;
}

// UI review is a bounded description of policy changes, never source content.
inline Json profile_review_summary(const std::optional<std::string>& before, const std::string& after) {
    const auto next = shin::context::parse_json_text(after, "candidate profile");
    Json result{{"schema_version", "shin.profile-review.v1"}, {"baseline_available", before.has_value()},
        {"changed", true}, {"can_accept", false}, {"checks_preserved", false},
        {"categories", Json::array()}, {"changed_fields", Json::array()}};
    if (!before) return result;
    const auto old = shin::context::parse_json_text(*before, "approved profile");
    if (!old.is_object() || !next.is_object()) return result;
    std::set<std::string> fields;
    for (const auto& [key, unused] : old.items()) fields.insert(key);
    for (const auto& [key, unused] : next.items()) fields.insert(key);
    std::map<std::string, Json> categories;
    bool understood = true;
    bool checks_preserved = true;
    for (const auto& key : fields) {
        auto a = old.value(key, Json());
        auto b = next.value(key, Json());
        if (a == b) continue;
        if (a.is_null() && b.is_array()) a = Json::array();
        if (b.is_null() && a.is_array()) b = Json::array();
        std::string category = "other";
        if (key == "rootFiles" || key == "optionalRootFiles" || key == "root_files" || key == "optional_root_files") category = "documents";
        else if (key == "documentIncludePrefixes" || key == "document_include_prefixes") category = "document-selection";
        else if (key == "sourceFactRoots" || key == "source_fact_roots") category = "sources";
        else if (key == "documentExcludePrefixes" || key == "document_exclude_prefixes") category = "exclusions";
        else if (key == "sourceExtensions" || key == "sourceFactExtensions" || key == "source_extensions" || key == "source_fact_extensions") category = "extensions";
        else if (key == "evalCases" || key == "packEvalCases" || key == "eval_cases" || key == "pack_eval_cases") category = "checks";
        else if (key == "summary" || key == "name" || key == "agentStartPaths" || key == "agent_start_paths") category = "guidance";
        // Unknown fields, including identity and resource policy, require an
        // operator review. A generic reassuring summary must never approve them.
        else understood = false;
        auto& counts = categories[category];
        if (counts.is_null()) counts = {{"id", category}, {"added", 0}, {"removed", 0}, {"changed", 0}};
        if (a.is_array() && b.is_array()) {
            if (category == "documents" || category == "document-selection" || category == "sources" || category == "exclusions") {
                // Short repository-relative examples help explain the scope;
                // arbitrary values and control characters never reach the UI.
                for (const auto* side : {"added_paths", "removed_paths"}) {
                    if (!counts.contains(side)) counts[side] = Json::array();
                    const auto& from = std::string_view(side) == "added_paths" ? b : a;
                    const auto& against = std::string_view(side) == "added_paths" ? a : b;
                    for (const auto& item : from) {
                        if (counts[side].size() == 5) break;
                        if (!item.is_string() || std::find(against.begin(), against.end(), item) != against.end()) continue;
                        const auto path = item.get<std::string>();
                        if (path.size() <= 160 && (path == "." || shin::context::profile_relative_path_valid(path)) &&
                            std::none_of(path.begin(), path.end(), [](unsigned char c) { return c < 32 || c == 127; }))
                            counts[side].push_back(path);
                    }
                }
            }
            for (const auto& item : a) if (std::find(b.begin(), b.end(), item) == b.end()) {
                counts["removed"] = counts["removed"].get<unsigned>() + 1;
                if (category == "checks") {
                    // Adding an exclusion to an existing Pack test strengthens
                    // that test only when every other requirement is identical.
                    const bool retained = item.is_object() && std::any_of(b.begin(), b.end(), [&](Json candidate) {
                        if (!candidate.is_object()) return false;
                        auto original = item;
                        const auto excluded = original.value("mustNotPaths", Json::array());
                        const auto now = candidate.value("mustNotPaths", Json::array());
                        original.erase("mustNotPaths"); candidate.erase("mustNotPaths");
                        return original == candidate && excluded.is_array() && now.is_array() &&
                            std::all_of(excluded.begin(), excluded.end(), [&](const Json& path) {
                                return std::find(now.begin(), now.end(), path) != now.end();
                            });
                    });
                    if (!retained) checks_preserved = false;
                }
            }
            for (const auto& item : b) if (std::find(a.begin(), a.end(), item) == a.end())
                counts["added"] = counts["added"].get<unsigned>() + 1;
        } else {
            counts["changed"] = counts["changed"].get<unsigned>() + 1;
            if (category == "checks") checks_preserved = false;
        }
        // Profile field identifiers only, never arbitrary profile values.
        if (key.size() <= 80 && std::all_of(key.begin(), key.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }) && result["changed_fields"].size() < 64) result["changed_fields"].push_back(key);
    }
    for (const auto& [unused, value] : categories) result["categories"].push_back(value);
    result["changed"] = old != next;
    result["checks_preserved"] = checks_preserved;
    result["can_accept"] = understood && old != next;
    return result;
}

inline shin::context::Profile approved_membership_profile(shin::context::Profile current, const std::string& text) {
    const auto value = shin::context::parse_json_text(text, "approved profile");
    const auto strings = [&](const char* name, const char* alias) {
        return value.contains(name) ? value.at(name).get<std::vector<std::string>>() :
            value.contains(alias) ? value.at(alias).get<std::vector<std::string>>() : std::vector<std::string>{};
    };
    current.root_files = strings("rootFiles", "root_files");
    current.optional_root_files = strings("optionalRootFiles", "optional_root_files");
    current.source_fact_roots = strings("sourceFactRoots", "source_fact_roots");
    if (strings("agentStartPaths", "agent_start_paths").empty()) current.agent_start_paths = current.root_files;
    current.config_hash = shin::context::fnv1a64(text);
    return current;
}
}
