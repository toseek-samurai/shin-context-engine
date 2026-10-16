#include "context_internal.h"
#include "generated_io.h"
#include "snapshot_verification.h"

namespace shin::context {
namespace {

constexpr int kDefaultDiffDetailLimit = 100;
constexpr int kMaxDiffDetailLimit = 1000;
constexpr std::size_t kMaxRecordRowsPerStore = 2'000'000;
constexpr std::uintmax_t kMaxRecordStoreBytes = 512ULL * 1024ULL * 1024ULL;

struct SnapshotDiffOptions {
    fs::path from;
    fs::path to;
    int limit = kDefaultDiffDetailLimit;
    bool strict = false;
};

struct RecordDigest {
    std::string hash;
    std::string path;
};

struct RecordChangeSummary {
    int added = 0;
    int removed = 0;
    int changed = 0;
};

struct MapChangeSummary {
    int added = 0;
    int removed = 0;
    int changed = 0;

    int total() const { return added + removed + changed; }
};

SnapshotDiffOptions parse_snapshot_diff_options(const std::vector<std::string>& args) {
    SnapshotDiffOptions options;
    bool from_seen = false;
    bool to_seen = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--from" || args[index] == "--to") {
            const bool is_from = args[index] == "--from";
            bool& seen = is_from ? from_seen : to_seen;
            if (seen || index + 1 >= args.size()) {
                throw_usage_error(seen ? "duplicate-option" : "missing-option-value",
                                  seen ? args[index] + " may be supplied only once."
                                       : args[index] + " requires an output directory.");
            }
            const std::string& option = args[index];
            ++index;
            fs::path value = canonical_snapshot_root(args[index], option);
            if (is_from) {
                options.from = std::move(value);
            } else {
                options.to = std::move(value);
            }
            seen = true;
        } else if (args[index] == "--limit") {
            if (index + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--limit requires a value.");
            }
            options.limit = parse_cli_integer(args[++index], "--limit", 1);
            if (options.limit > kMaxDiffDetailLimit) {
                throw_usage_error("invalid-option-value", "snapshot-diff --limit must not exceed 1000.");
            }
        } else if (args[index] == "--strict") {
            options.strict = true;
        } else {
            throw_usage_error("unsupported-option",
                              "snapshot-diff only accepts --from, --to, --limit, and --strict.");
        }
    }
    if (!from_seen || !to_seen) {
        throw_usage_error("missing-option", "snapshot-diff requires --from OUT_DIR and --to OUT_DIR.");
    }
    return options;
}

std::string record_identity(const std::string& store, const Json& row) {
    if (store == "sections") {
        return Json::array({json_string_value(row, "document_id"), json_string_value(row, "row_type"),
                            json_string_value(row, "anchor"), json_int_value(row, "chunk_index", 0),
                            json_int_value(row, "start_line", 0), json_int_value(row, "end_line", 0)}).dump();
    }
    if (store == "links") {
        return Json::array({json_string_value(row, "path"), json_string_value(row, "kind"),
                            json_int_value(row, "line", 0), json_string_value(row, "target")}).dump();
    }
    return json_string_value(row, "id");
}

std::map<std::string, RecordDigest> load_record_digests(const VerifiedSnapshot& snapshot,
                                                        const std::string& relative_path,
                                                        const std::string& store) {
    auto file = snapshot.files.find(relative_path);
    if (file == snapshot.files.end()) {
        return {};
    }
    if (file->second.bytes > kMaxRecordStoreBytes) {
        throw CliError("snapshot-record-store-too-large", "resource",
                       "Record-level diff store exceeds 512 MiB: " + relative_path, 1, false,
                       "Use file-level snapshot evidence or split the generated corpus.");
    }
    std::map<std::string, RecordDigest> rows;
    std::size_t row_count = 0;
    for_each_jsonl_line_if_exists(snapshot.out_root / relative_path, [&](const std::string& line) {
        if (++row_count > kMaxRecordRowsPerStore) {
            throw CliError("snapshot-record-count-exceeded", "resource",
                           "Record-level diff store exceeds two million rows: " + relative_path, 1,
                           false, "Use file-level snapshot evidence or split the generated corpus.");
        }
        const Json row = parse_json_line(line, relative_path);
        if (!row.is_object()) {
            throw_index_error("snapshot-record-invalid",
                              "Generated snapshot record must be an object: " + relative_path, false);
        }
        const std::string identity = record_identity(store, row);
        if (identity.empty()) {
            throw_index_error("snapshot-record-identity-missing",
                              "Generated snapshot record has no stable identity: " + relative_path, false);
        }
        RecordDigest digest{fnv1a64(line), json_string_value(row, "path")};
        if (!rows.emplace(identity, std::move(digest)).second) {
            throw_index_error("snapshot-record-identity-duplicate",
                              "Generated snapshot record identity is duplicated: " + relative_path, false);
        }
    });
    return rows;
}

void append_record_changes(const VerifiedSnapshot& from, const VerifiedSnapshot& to,
                           const std::string& relative_path, const std::string& store,
                           int detail_limit, RecordChangeSummary& summary,
                           std::map<std::string, RecordChangeSummary>& by_store,
                           Json& details) {
    const auto from_rows = load_record_digests(from, relative_path, store);
    const auto to_rows = load_record_digests(to, relative_path, store);
    auto left = from_rows.begin();
    auto right = to_rows.begin();
    auto append = [&](const std::string& change, const std::string& key,
                      const std::string& path) {
        RecordChangeSummary& store_summary = by_store[store];
        if (change == "added") {
            ++summary.added;
            ++store_summary.added;
        } else if (change == "removed") {
            ++summary.removed;
            ++store_summary.removed;
        } else {
            ++summary.changed;
            ++store_summary.changed;
        }
        if (details.size() < static_cast<std::size_t>(detail_limit)) {
            details.push_back({{"store", store}, {"change", change}, {"key", key},
                               {"path", path.empty() ? Json(nullptr) : Json(path)}});
        }
    };
    while (left != from_rows.end() || right != to_rows.end()) {
        if (right == to_rows.end() || (left != from_rows.end() && left->first < right->first)) {
            append("removed", left->first, left->second.path);
            ++left;
        } else if (left == from_rows.end() || right->first < left->first) {
            append("added", right->first, right->second.path);
            ++right;
        } else {
            if (left->second.hash != right->second.hash) {
                append("changed", left->first,
                       right->second.path.empty() ? left->second.path : right->second.path);
            }
            ++left;
            ++right;
        }
    }
}

Json compare_digest_maps(const std::map<std::string, std::string>& from,
                         const std::map<std::string, std::string>& to,
                         int detail_limit,
                         MapChangeSummary& summary) {
    Json details = Json::array();
    auto left = from.begin();
    auto right = to.begin();
    auto append = [&](const std::string& change, const std::string& key) {
        if (change == "added") {
            ++summary.added;
        } else if (change == "removed") {
            ++summary.removed;
        } else {
            ++summary.changed;
        }
        if (details.size() < static_cast<std::size_t>(detail_limit)) {
            details.push_back({{"change", change}, {"key", key}});
        }
    };
    while (left != from.end() || right != to.end()) {
        if (right == to.end() || (left != from.end() && left->first < right->first)) {
            append("removed", left->first);
            ++left;
        } else if (left == from.end() || right->first < left->first) {
            append("added", right->first);
            ++right;
        } else {
            if (left->second != right->second) {
                append("changed", left->first);
            }
            ++left;
            ++right;
        }
    }
    return details;
}

std::map<std::string, std::string> json_object_digests(const Json& object) {
    std::map<std::string, std::string> digests;
    for (auto field = object.begin(); field != object.end(); ++field) {
        digests.emplace(field.key(), fnv1a64(field->dump()));
    }
    return digests;
}

}  // namespace

int run_snapshot_diff(const std::vector<std::string>& args) {
    const SnapshotDiffOptions options = parse_snapshot_diff_options(args);
    Json output;
    bool identical = false;
    {
        std::vector<fs::path> lock_roots = {options.from, options.to};
        std::sort(lock_roots.begin(), lock_roots.end(), [](const fs::path& left, const fs::path& right) {
            return left.generic_string() < right.generic_string();
        });
        lock_roots.erase(std::unique(lock_roots.begin(), lock_roots.end()), lock_roots.end());
        std::vector<std::unique_ptr<shin::generated_io::IndexLock>> locks;
        locks.reserve(lock_roots.size());
        for (const fs::path& root : lock_roots) {
            locks.push_back(std::make_unique<shin::generated_io::IndexLock>(
                root, shin::generated_io::LockMode::shared_existing));
        }

        const VerifiedSnapshot from = load_verified_snapshot(options.from);
        const VerifiedSnapshot to = load_verified_snapshot(options.to);
        identical = from.manifest_content == to.manifest_content;
        const bool same_snapshot_id = from.snapshot_id == to.snapshot_id;
        const bool compatible = from.schema_version == to.schema_version &&
                                from.generator_version == to.generator_version &&
                                from.profile == to.profile;

        std::map<std::string, std::string> from_metadata = {
            {"schema_version", fnv1a64(from.schema_version)},
            {"generator_version", fnv1a64(from.generator_version)},
            {"profile", fnv1a64(from.profile)},
            {"profile_config_hash", fnv1a64(from.profile_config_hash)}
        };
        std::map<std::string, std::string> to_metadata = {
            {"schema_version", fnv1a64(to.schema_version)},
            {"generator_version", fnv1a64(to.generator_version)},
            {"profile", fnv1a64(to.profile)},
            {"profile_config_hash", fnv1a64(to.profile_config_hash)}
        };
        MapChangeSummary metadata_summary;
        MapChangeSummary row_count_summary;
        MapChangeSummary source_summary;
        MapChangeSummary source_fact_summary;
        Json metadata_changes = compare_digest_maps(from_metadata, to_metadata, options.limit,
                                                     metadata_summary);
        Json row_count_changes = compare_digest_maps(json_object_digests(from.row_counts),
                                                      json_object_digests(to.row_counts), options.limit,
                                                      row_count_summary);
        Json source_changes = compare_digest_maps(from.sources, to.sources, options.limit,
                                                   source_summary);
        Json source_fact_changes = compare_digest_maps(from.source_facts, to.source_facts,
                                                        options.limit, source_fact_summary);

        Json file_changes = Json::array();
        int files_added = 0;
        int files_removed = 0;
        int files_changed = 0;
        std::set<std::string> file_paths;
        for (const auto& [path, file] : from.files) {
            (void)file;
            file_paths.insert(path);
        }
        for (const auto& [path, file] : to.files) {
            (void)file;
            file_paths.insert(path);
        }
        for (const std::string& path : file_paths) {
            const auto left = from.files.find(path);
            const auto right = to.files.find(path);
            std::string change;
            if (left == from.files.end()) {
                change = "added";
                ++files_added;
            } else if (right == to.files.end()) {
                change = "removed";
                ++files_removed;
            } else if (left->second.content_hash != right->second.content_hash ||
                       left->second.bytes != right->second.bytes) {
                change = "changed";
                ++files_changed;
            }
            if (!change.empty() && file_changes.size() < static_cast<std::size_t>(options.limit)) {
                file_changes.push_back({{"key", path}, {"change", change}});
            }
        }
        if (!identical) {
            ++files_changed;
            if (file_changes.size() < static_cast<std::size_t>(options.limit)) {
                file_changes.push_back({{"key", (kGeneratedDir / "manifest.json").generic_string()},
                                        {"change", "changed"}});
            }
        }

        static const std::vector<std::pair<std::string, std::string>> record_stores = {
            {"documents.jsonl", "documents"}, {"sections.jsonl", "sections"},
            {"links.jsonl", "links"},         {"topics.jsonl", "topics"},
            {"decisions.jsonl", "decisions"}, {"contracts.jsonl", "contracts"},
            {"runbooks.jsonl", "runbooks"},   {"facts.jsonl", "facts"}
        };
        RecordChangeSummary record_summary;
        std::map<std::string, RecordChangeSummary> records_by_store;
        Json record_changes = Json::array();
        for (const auto& [file, store] : record_stores) {
            const std::string path = (kGeneratedDir / file).generic_string();
            const auto left = from.files.find(path);
            const auto right = to.files.find(path);
            if (left != from.files.end() && right != to.files.end() &&
                left->second.content_hash == right->second.content_hash) {
                continue;
            }
            append_record_changes(from, to, path, store, options.limit, record_summary,
                                  records_by_store, record_changes);
        }

        Json by_store = Json::object();
        for (const auto& [store, summary] : records_by_store) {
            by_store[store] = {
                {"added", summary.added}, {"removed", summary.removed}, {"changed", summary.changed}
            };
        }
        Json details = Json::array();
        auto append_details = [&](const std::string& category, const Json& changes) {
            for (const Json& change : changes) {
                if (details.size() >= static_cast<std::size_t>(options.limit)) {
                    break;
                }
                Json detail = change;
                detail["category"] = category;
                details.push_back(std::move(detail));
            }
        };
        append_details("metadata", metadata_changes);
        append_details("row-count", row_count_changes);
        append_details("source", source_changes);
        append_details("source-fact", source_fact_changes);
        append_details("generated-file", file_changes);
        append_details("syntactic-record", record_changes);
        const int detail_total = metadata_summary.total() + row_count_summary.total() +
                                 source_summary.total() + source_fact_summary.total() +
                                 files_added + files_removed + files_changed +
                                 record_summary.added + record_summary.removed + record_summary.changed;

        output = make_result_document("shin.snapshot-diff.v1", "snapshot-diff", identical);
        output["strict"] = options.strict;
        output["same_snapshot_id"] = same_snapshot_id;
        output["identical"] = identical;
        output["compatible"] = compatible;
        output["from"] = verified_snapshot_description(from);
        output["to"] = verified_snapshot_description(to);
        output["summary"] = {
            {"metadata_changed", metadata_summary.total()},
            {"row_counts_changed", row_count_summary.total()},
            {"sources_added", source_summary.added},
            {"sources_removed", source_summary.removed},
            {"sources_changed", source_summary.changed},
            {"source_facts_added", source_fact_summary.added},
            {"source_facts_removed", source_fact_summary.removed},
            {"source_facts_changed", source_fact_summary.changed},
            {"files_added", files_added},
            {"files_removed", files_removed},
            {"files_changed", files_changed},
            {"syntactic_records_added", record_summary.added},
            {"syntactic_records_removed", record_summary.removed},
            {"syntactic_records_changed", record_summary.changed}
        };
        output["syntactic_records_by_store"] = std::move(by_store);
        output["syntactic_record_identity"] = {
            {"policy", "generated-row-identity-v1"},
            {"semantic_change_claim", false},
            {"note", "Line, anchor, move, or generated-id changes may appear as remove plus add."}
        };
        output["detail_limit"] = options.limit;
        output["details_returned"] = details.size();
        output["details_omitted"] = detail_total - static_cast<int>(details.size());
        output["details"] = std::move(details);
    }
    emit_json_document(std::cout, output);
    return options.strict && !identical ? 1 : 0;
}

}  // namespace shin::context
