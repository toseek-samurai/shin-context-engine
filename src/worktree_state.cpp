#include "context_internal.h"
#include "worktree_state.h"
#include "project_files.h"
#include "source_read.h"
#include <ranges>

namespace shin::context {
namespace {

using SourceState = WorktreeSourceState;
using CurrentInventory = WorktreeInventory;

template <typename Work>
auto observe_source(const std::string& operation, Work&& work) {
    try {
        return work();
    } catch (const CliError&) {
        throw;
    } catch (const std::system_error& error) {
        throw_source_observation_error(operation, error.code());
    }
}

struct ChangeCounts {
    int added = 0;
    int removed = 0;
    int changed = 0;

    int total() const { return added + removed + changed; }
};

std::uintmax_t required_byte_count(const Json& row,
                                   const std::string& field,
                                   const std::string& location) {
    const Json* value = find_json_field(row, field);
    if (value == nullptr || !value->is_number_integer()) {
        throw_index_error("worktree-manifest-invalid",
                          location + "." + field + " must be a non-negative integer.", false);
    }
    try {
        if (value->is_number_unsigned()) {
            return value->get<std::uintmax_t>();
        }
        const std::int64_t parsed = value->get<std::int64_t>();
        if (parsed < 0) {
            throw std::out_of_range("negative");
        }
        return static_cast<std::uintmax_t>(parsed);
    } catch (const std::exception&) {
        throw_index_error("worktree-manifest-invalid",
                          location + "." + field + " is outside the supported range.", false);
    }
}

std::map<std::string, SourceState> recorded_sources(const Json& manifest,
                                                    const std::string& field) {
    const Json* rows = find_json_field(manifest, field);
    if (rows == nullptr || !rows->is_array()) {
        throw_index_error("worktree-manifest-invalid",
                          "Generated manifest " + field + " must be an array.", false);
    }
    std::map<std::string, SourceState> sources;
    for (const Json& row : *rows) {
        if (!row.is_object()) {
            throw_index_error("worktree-manifest-invalid",
                              "Generated manifest " + field + " row must be an object.", false);
        }
        SourceState source;
        source.path = json_string_value(row, "path");
        source.status = json_string_value(row, "status");
        source.content_hash = json_string_value(row, "content_hash");
        source.byte_count = required_byte_count(row, "bytes", field);
        if (!profile_relative_path_valid(source.path) ||
            !sources.emplace(source.path, std::move(source)).second) {
            throw_index_error("worktree-manifest-invalid",
                              "Generated manifest " + field +
                                  " has an invalid or duplicate path.", false);
        }
    }
    return sources;
}

std::map<std::string, SourceState> current_document_sources(const RuntimeOptions& options,
                                                            const std::vector<std::string>& paths) {
    std::vector<SourceState> states(paths.size());
    parallel_for_indices(paths.size(), [&](std::size_t index) {
        const FileFingerprint fingerprint = source_fingerprint_file(
            options.root / paths[index], "read-source-content");
        states[index] = {paths[index], "", fingerprint.content_hash,
                         fingerprint.metadata.byte_count, fingerprint.metadata, true};
    });
    std::map<std::string, SourceState> result;
    for (SourceState& state : states) {
        result.emplace(state.path, std::move(state));
    }
    return result;
}

std::map<std::string, SourceState> current_source_fact_sources(
    const RuntimeOptions& options,
    const SourceFactPathResult& paths,
    DoctorSourceAudit* audit) {
    std::vector<SourceState> states(paths.paths.size());
    const auto observe = [&](std::size_t index) {
        const std::string& path = paths.paths[index];
        const fs::path full_path = options.root / path;
        const FileMetadata metadata = source_file_metadata(full_path, "read-source-metadata");
        if (options.profile.source_fact_max_file_bytes > 0 &&
            metadata.byte_count >
                static_cast<std::uintmax_t>(options.profile.source_fact_max_file_bytes)) {
            states[index] = {path, "skipped-large", "", metadata.byte_count, metadata, true};
            return;
        }
        FileFingerprint fingerprint;
        if (audit && !path_matches_any_profile_prefix(path, options.profile.secret_scan_exclude_prefixes)) {
            const auto content = read_project_text(options.root, path,
                static_cast<std::size_t>(options.profile.source_fact_max_file_bytes));
            if (content.size() != metadata.byte_count ||
                source_file_metadata(full_path, "verify-source-metadata") != metadata)
                throw_source_changed_during_verification("verify-source-metadata");
            fingerprint = {metadata, fnv1a64(content)};
            const auto hit = source_range_secret(content, path);
            if (hit.found) audit->findings.push_back({path, fingerprint.content_hash, hit.kind});
            ++audit->scanned_files;
        } else {
            fingerprint = source_fingerprint_file(full_path, "read-source-content");
            if (audit) ++audit->excluded_files;
        }
        states[index] = {path, "scanned", fingerprint.content_hash,
                         fingerprint.metadata.byte_count, fingerprint.metadata, true};
    };
    // Like the automatic doctor scan, retain at most one full source body.
    if (audit) {
        for (std::size_t i = 0; i < paths.paths.size(); ++i) observe(i);
    } else parallel_for_indices(paths.paths.size(), observe);
    std::map<std::string, SourceState> result;
    for (SourceState& state : states) {
        result.emplace(state.path, std::move(state));
    }
    return result;
}

struct Selection {
    std::vector<std::string> documents;
    SourceFactPathResult facts;
    ProjectIgnoreBinding exclusions;
};

Selection select_worktree(const RuntimeOptions& options, const char* operation) {
    if (options.profile.automatic_project_files) {
        auto project = ProjectFiles::scan(options.profile, options.root);
        return {std::move(project.documents), std::move(project.sources), std::move(project.exclusions)};
    }
    auto documents = observe_source(operation, [&] { return enumerate_source_paths(options.profile, options.root); });
    return {std::move(documents), enumerate_source_fact_paths(options.profile, options.root), {}};
}

Selection observed_selection(const WorktreeInventory& inventory) {
    Selection result;
    result.documents.reserve(inventory.documents.size());
    result.facts.paths.reserve(inventory.source_facts.size());
    for (const auto& entry : inventory.documents) result.documents.push_back(entry.first);
    for (const auto& entry : inventory.source_facts) result.facts.paths.push_back(entry.first);
    result.facts.candidate_file_count = inventory.source_fact_candidate_files;
    result.facts.capped_file_count = inventory.source_fact_capped_files;
    return result;
}

void verify_inventory_stable(const RuntimeOptions& options,
                             const CurrentInventory& inventory,
                             const Selection& before) {
    const auto after = select_worktree(options, "enumerate-source-entry");
    if (after.documents != before.documents || after.facts.paths != before.facts.paths ||
        after.facts.candidate_file_count != before.facts.candidate_file_count ||
        after.facts.capped_file_count != before.facts.capped_file_count) {
        throw_source_changed_during_verification("enumerate-source-entry");
    }
    require_worktree_metadata_stable(options, inventory);
}

void verify_source_metadata(const RuntimeOptions& options, const WorktreeInventory& inventory) {
    if (options.profile.automatic_project_files && !inventory.exclusions.current(options.root))
        throw_source_changed_during_verification("verify-project-exclusions");
    auto verify_states = [&](const std::map<std::string, SourceState>& states) {
        for (const auto& [path, state] : states) {
            try {
                if (!state.has_metadata || source_file_metadata(
                        options.root / path, "verify-source-metadata") != state.metadata) {
                    throw_source_changed_during_verification("verify-source-metadata");
                }
            } catch (const CliError&) {
                throw;
            }
        }
    };
    verify_states(inventory.documents);
    verify_states(inventory.source_facts);
}

CurrentInventory load_current_inventory(const RuntimeOptions& options, DoctorSourceAudit* audit = nullptr) {
    CurrentInventory inventory;
    const auto selection = select_worktree(options, "enumerate-source-root");
    inventory.source_facts = current_source_fact_sources(options, selection.facts, audit);
    if (options.profile.automatic_project_files) {
        inventory.exclusions = selection.exclusions;
        for (const auto& path : selection.documents) {
            auto document = inventory.source_facts.at(path);
            document.status.clear();
            inventory.documents.emplace(path, std::move(document));
        }
    } else inventory.documents = current_document_sources(options, selection.documents);
    inventory.source_fact_candidate_files = selection.facts.candidate_file_count;
    inventory.source_fact_capped_files = selection.facts.capped_file_count;
    verify_inventory_stable(options, inventory, selection);
    return inventory;
}

template <typename Documents, typename Facts, typename ReadMetadata>
std::string metadata_payload(const Documents& documents, const Facts& facts,
                             const ProjectIgnoreBinding& exclusions, int candidates, int capped, ReadMetadata read) {
    constexpr std::size_t limit = 4 * 1024 * 1024;
    std::string out;
    const auto number = [&](const auto value) {
        out.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    const auto path = [&](const std::string& value) {
        number(value.size());
        out.append(value);
    };
    const auto fits = [&](std::size_t text_bytes, std::size_t field_bytes) {
        const auto remaining = limit - out.size();
        return field_bytes <= remaining && text_bytes <= remaining - field_bytes;
    };
    if (!fits(exclusions.revision.size(), 2 * sizeof(int) + 2 * sizeof(std::size_t))) return {};
    number(candidates);
    number(capped);
    path(exclusions.revision);
    number(exclusions.directories.size());
    for (const auto& directory : exclusions.directories) {
        if (!fits(directory.size(), sizeof(std::size_t))) return {};
        path(directory);
    }
    auto doc = documents.begin();
    auto fact = facts.begin();
    while (doc != documents.end() || fact != facts.end()) {
        const bool document = doc != documents.end() && (fact == facts.end() || *doc <= *fact);
        const bool source = fact != facts.end() && (doc == documents.end() || *fact <= *doc);
        const auto& name = document ? *doc : *fact;
        if (document) ++doc;
        if (source) ++fact;
        constexpr auto fields = sizeof(std::size_t) + sizeof(unsigned char) + sizeof(FileMetadata);
        if (!fits(name.size(), fields)) return {};
        const auto metadata = read(name, document, source);
        number(static_cast<unsigned char>((document ? 1 : 0) | (source ? 2 : 0)));
        path(name);
        number(metadata.byte_count);
        number(metadata.modified_ticks);
        number(metadata.changed_ticks);
        number(metadata.device_id);
        number(metadata.file_id);
    }
    return out;
}

bool source_state_changed(const SourceState& baseline,
                          const SourceState& current,
                          bool compare_status) {
    return baseline.content_hash != current.content_hash ||
           baseline.byte_count != current.byte_count ||
           (compare_status && baseline.status != current.status);
}

Json state_json(const SourceState* state) {
    if (state == nullptr) {
        return nullptr;
    }
    Json result = {
        {"content_hash", state->content_hash.empty() ? Json(nullptr) : Json(state->content_hash)},
        {"bytes", state->byte_count}
    };
    if (!state->status.empty()) {
        result["status"] = state->status;
    }
    return result;
}

void append_source_changes(const std::string& scope,
                           const std::map<std::string, SourceState>& baseline,
                           const std::map<std::string, SourceState>& current,
                           bool compare_status,
                           int detail_limit,
                           ChangeCounts& counts,
                           Json& details) {
    auto left = baseline.begin();
    auto right = current.begin();
    auto append = [&](const std::string& change,
                      const std::string& path,
                      const SourceState* baseline_state,
                      const SourceState* current_state) {
        if (change == "added") {
            ++counts.added;
        } else if (change == "removed") {
            ++counts.removed;
        } else {
            ++counts.changed;
        }
        if (details.size() < static_cast<std::size_t>(detail_limit)) {
            details.push_back({
                {"category", scope},
                {"change", change},
                {"path", path},
                {"baseline", state_json(baseline_state)},
                {"worktree", state_json(current_state)}
            });
        }
    };
    while (left != baseline.end() || right != current.end()) {
        if (right == current.end() || (left != baseline.end() && left->first < right->first)) {
            append("removed", left->first, &left->second, nullptr);
            ++left;
        } else if (left == baseline.end() || right->first < left->first) {
            append("added", right->first, nullptr, &right->second);
            ++right;
        } else {
            if (source_state_changed(left->second, right->second, compare_status)) {
                append("changed", left->first, &left->second, &right->second);
            }
            ++left;
            ++right;
        }
    }
}

Json inventory_identity(const RuntimeOptions& options,
                        const CurrentInventory& inventory) {
    Json identity = {
        {"profile", options.profile.id},
        {"profile_config_hash", profile_config_hash(options.profile)},
        {"source_fact_candidate_files", inventory.source_fact_candidate_files},
        {"source_fact_capped_files", inventory.source_fact_capped_files},
        {"sources", Json::array()}
    };
    auto append = [&](const std::string& scope,
                      const std::map<std::string, SourceState>& states) {
        for (const auto& [path, state] : states) {
            identity["sources"].push_back({
                {"scope", scope},
                {"path", path},
                {"status", state.status},
                {"content_hash", state.content_hash.empty() ? Json(nullptr)
                                                               : Json(state.content_hash)},
                {"bytes", state.byte_count}
            });
        }
    };
    append("document", inventory.documents);
    append("source-fact", inventory.source_facts);
    if (!inventory.exclusions.revision.empty()) identity["project_exclusions"] = inventory.exclusions.revision;
    return identity;
}

int manifest_nonnegative_int(const Json& object,
                             const std::string& field,
                             const std::string& location) {
    const Json* value = find_json_field(object, field);
    if (value == nullptr || !value->is_number_integer()) {
        throw_index_error("worktree-manifest-invalid",
                          location + "." + field + " must be a non-negative integer.", false);
    }
    try {
        const std::uint64_t parsed = value->is_number_unsigned()
                                         ? value->get<std::uint64_t>()
                                         : static_cast<std::uint64_t>(value->get<std::int64_t>());
        if ((!value->is_number_unsigned() && value->get<std::int64_t>() < 0) ||
            parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            throw std::out_of_range("range");
        }
        return static_cast<int>(parsed);
    } catch (const std::exception&) {
        throw_index_error("worktree-manifest-invalid",
                          location + "." + field + " is outside the supported range.", false);
    }
}

}  // namespace

WorktreeInventory observe_worktree(const RuntimeOptions& options, DoctorSourceAudit* audit) {
    return load_current_inventory(options, audit);
}

void require_worktree_metadata_stable(const RuntimeOptions& options, const WorktreeInventory& inventory) {
    verify_source_metadata(options, inventory);
}

void require_worktree_stable(const RuntimeOptions& options, const WorktreeInventory& inventory) {
    verify_inventory_stable(options, inventory, observed_selection(inventory));
}

std::string worktree_metadata_payload(const WorktreeInventory& inventory) {
    return metadata_payload(std::views::keys(inventory.documents), std::views::keys(inventory.source_facts),
        inventory.exclusions, inventory.source_fact_candidate_files, inventory.source_fact_capped_files,
        [&](const std::string& path, bool document, bool source) {
            const auto& state = source ? inventory.source_facts.at(path) : inventory.documents.at(path);
            if (!state.has_metadata) throw_source_changed_during_verification("verify-source-metadata");
            if (document && source) {
                const auto& other = inventory.documents.at(path);
                if (!other.has_metadata || other.metadata != state.metadata)
                    throw_source_changed_during_verification("verify-source-metadata");
            }
            return state.metadata;
        });
}

WorktreeMetadataObservation observe_worktree_metadata(const RuntimeOptions& options) {
    auto selection = select_worktree(options, "enumerate-source-entry");
    auto payload = metadata_payload(selection.documents, selection.facts.paths, selection.exclusions,
        selection.facts.candidate_file_count, selection.facts.capped_file_count,
        [&](const std::string& path, bool, bool) {
            return source_file_metadata(options.root / path, "verify-source-metadata");
        });
    if (options.profile.automatic_project_files && !selection.exclusions.current(options.root))
        throw_source_changed_during_verification("verify-project-exclusions");
    return {std::move(payload), std::move(selection.facts.paths)};
}

Json worktree_identity(const RuntimeOptions& options, const WorktreeInventory& inventory) {
    return inventory_identity(options, inventory);
}

bool worktree_observation_complete(const WorktreeInventory& inventory) {
    return inventory.source_fact_capped_files == 0 &&
           std::all_of(inventory.source_facts.begin(), inventory.source_facts.end(),
                       [](const auto& entry) { return entry.second.status == "scanned"; });
}

Json compare_worktree(const RuntimeOptions& options, const Json& manifest,
                      const WorktreeInventory& inventory, int detail_limit) {
    if (json_string_value(manifest, "profile") != options.profile.id) {
        throw CliError("worktree-diff-profile-mismatch", "configuration",
                       "Generated snapshot profile does not match the active profile.", 1, false,
                       "Select the profile that owns the published index or rebuild it.");
    }

    const std::map<std::string, SourceState> baseline_documents =
        recorded_sources(manifest, "sources");
    const std::map<std::string, SourceState> baseline_source_facts =
        recorded_sources(manifest, "source_fact_sources");
    const int manifest_source_count =
        manifest_nonnegative_int(manifest, "source_count", "manifest");
    if (manifest_source_count != static_cast<int>(baseline_documents.size())) {
        throw_index_error("worktree-manifest-invalid",
                          "Generated manifest source_count does not match sources.", false);
    }
    const Json* source_fact_counts = find_json_field(manifest, "source_fact_counts");
    if (source_fact_counts == nullptr || !source_fact_counts->is_object()) {
        throw_index_error("worktree-manifest-invalid",
                          "Generated manifest source_fact_counts must be an object.", false);
    }
    const int baseline_candidate_files = manifest_nonnegative_int(
        *source_fact_counts, "candidate_files", "source_fact_counts");
    const int baseline_capped_files = manifest_nonnegative_int(
        *source_fact_counts, "capped_files", "source_fact_counts");

    const std::string current_profile_hash = profile_config_hash(options.profile);
    const std::string generator = json_string_value(manifest, "generator_version");
    const std::string profile_hash = json_string_value(manifest, "profile_config_hash");
    const bool generator_current = generator == kGeneratorVersion;
    const bool profile_config_current = profile_hash == current_profile_hash;
    const bool source_fact_inventory_current =
        baseline_candidate_files == inventory.source_fact_candidate_files &&
        baseline_capped_files == inventory.source_fact_capped_files;

    Json details = Json::array();
    int metadata_changes = 0;
    auto append_metadata_change = [&](const std::string& key,
                                      const Json& baseline,
                                      const Json& worktree) {
        ++metadata_changes;
        if (details.size() < static_cast<std::size_t>(detail_limit)) {
            details.push_back({
                {"category", "metadata"},
                {"change", "changed"},
                {"key", key},
                {"baseline", baseline},
                {"worktree", worktree}
            });
        }
    };
    if (options.profile.automatic_project_files) {
        const auto previous = manifest.value("project_files", Json::object()).value("exclusion_revision", "");
        if (previous != inventory.exclusions.revision)
            append_metadata_change("project_exclusions", previous, inventory.exclusions.revision);
    }
    if (!generator_current) {
        append_metadata_change("generator_version", generator, kGeneratorVersion);
    }
    if (!profile_config_current) {
        append_metadata_change("profile_config_hash", profile_hash,
                               current_profile_hash);
    }
    const Json source_limits{{"max_files", options.profile.source_fact_max_files},
                             {"max_file_bytes", options.profile.source_fact_max_file_bytes}};
    const auto* baseline_limits = find_json_field(manifest, "source_fact_limits");
    if (profile_config_current && (!baseline_limits || *baseline_limits != source_limits)) {
        append_metadata_change("source_fact_limits", baseline_limits ? *baseline_limits : Json(nullptr), source_limits);
    }
    const auto profile_location = manifest_profile_config_identity(options.profile);
    if (json_string_value(manifest, "profile_config") != profile_location) {
        append_metadata_change("profile_config", json_string_value(manifest, "profile_config"), profile_location);
    }
    if (!source_fact_inventory_current) {
        append_metadata_change(
            "source_fact_inventory",
            {{"candidate_files", baseline_candidate_files},
             {"capped_files", baseline_capped_files}},
            {{"candidate_files", inventory.source_fact_candidate_files},
             {"capped_files", inventory.source_fact_capped_files}});
    }

    ChangeCounts document_changes;
    ChangeCounts source_fact_changes;
    append_source_changes("document", baseline_documents, inventory.documents, false,
                          detail_limit, document_changes, details);
    append_source_changes("source-fact", baseline_source_facts, inventory.source_facts, true,
                          detail_limit, source_fact_changes, details);

    const int total_changes = metadata_changes + document_changes.total() +
                              source_fact_changes.total();
    const bool current = total_changes == 0;
    const Json identity = inventory_identity(options, inventory);

    Json output = Json::object();
    output["current"] = current;
    output["worktree"] = {
        {"root", options.root.generic_string()},
        {"worktree_id", fnv1a64(identity.dump())},
        {"profile", options.profile.id},
        {"profile_config_hash", current_profile_hash},
        {"documents", inventory.documents.size()},
        {"source_facts", inventory.source_facts.size()},
        {"source_fact_candidate_files", inventory.source_fact_candidate_files},
        {"source_fact_capped_files", inventory.source_fact_capped_files}
    };
    output["comparison"] = {
        {"policy", "profile-owned-source-content-v1"},
        {"snapshot_integrity", "verified"},
        {"git_required", false},
        {"read_only", true},
        {"semantic_change_claim", false},
        {"generator_current", generator_current},
        {"profile_config_current", profile_config_current},
        {"source_fact_inventory_current", source_fact_inventory_current}
    };
    output["summary"] = {
        {"metadata_changed", metadata_changes},
        {"documents_added", document_changes.added},
        {"documents_removed", document_changes.removed},
        {"documents_changed", document_changes.changed},
        {"source_facts_added", source_fact_changes.added},
        {"source_facts_removed", source_fact_changes.removed},
        {"source_facts_changed", source_fact_changes.changed},
        {"total_changes", total_changes}
    };
    output["detail_limit"] = detail_limit;
    output["details_returned"] = details.size();
    output["details_omitted"] = total_changes - static_cast<int>(details.size());
    output["details"] = std::move(details);
    return output;
}

}  // namespace shin::context
