#include "context_internal.h"
#include "generated_io.h"
#include "snapshot_verification.h"

namespace shin::context {
namespace {

constexpr std::size_t kMaxSnapshotFiles = 1024;
constexpr std::size_t kMaxSnapshotFilesystemEntries = 2048;
constexpr std::uintmax_t kMaxSnapshotManifestBytes = 64ULL * 1024ULL * 1024ULL;

void require_regular_snapshot_file(const fs::path& path,
                                   const std::string& label) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (error || !fs::is_regular_file(status) || fs::is_symlink(status)) {
        throw CliError("snapshot-file-invalid", "index",
                       label + " must be a non-symlink regular file: " + path.string(),
                       1, false,
                       "Rebuild and check the generated snapshot before comparing it.");
    }
}

bool generated_snapshot_path_valid(const std::string& path) {
    if (!profile_relative_path_valid(path)) {
        return false;
    }
    const std::string prefix = (kGeneratedDir / "").generic_string();
    return starts_with(path, prefix) && path.size() > prefix.size();
}

std::uintmax_t required_nonnegative_size(const Json& object,
                                         const std::string& field,
                                         const std::string& location) {
    const Json* value = find_json_field(object, field);
    if (value == nullptr || !value->is_number_integer()) {
        throw_index_error("snapshot-manifest-invalid",
                          location + "." + field +
                              " must be a non-negative integer.",
                          false);
    }
    try {
        if (value->is_number_unsigned()) {
            return value->get<std::uintmax_t>();
        }
        const long long parsed = value->get<long long>();
        if (parsed < 0) {
            throw std::out_of_range("negative");
        }
        return static_cast<std::uintmax_t>(parsed);
    } catch (const std::exception&) {
        throw_index_error("snapshot-manifest-invalid",
                          location + "." + field +
                              " is outside the supported range.",
                          false);
    }
}

std::map<std::string, std::string> load_manifest_entities(
    const Json& manifest,
    const std::string& field) {
    std::map<std::string, std::string> entities;
    const Json* rows = find_json_field(manifest, field);
    if (rows == nullptr || !rows->is_array()) {
        throw_index_error("snapshot-manifest-invalid",
                          "Generated manifest " + field + " must be an array.",
                          false);
    }
    for (const Json& row : *rows) {
        if (!row.is_object()) {
            throw_index_error("snapshot-manifest-invalid",
                              "Generated manifest " + field +
                                  " row must be an object.",
                              false);
        }
        const std::string path = json_string_value(row, "path");
        if (!profile_relative_path_valid(path) ||
            !entities.emplace(path, fnv1a64(row.dump())).second) {
            throw_index_error("snapshot-manifest-invalid",
                              "Generated manifest " + field +
                                  " has an invalid or duplicate path.",
                              false);
        }
    }
    return entities;
}

std::set<std::string> enumerate_snapshot_files(const fs::path& out_root,
                                               const fs::path& generated_root) {
    std::set<std::string> files;
    std::error_code error;
    fs::recursive_directory_iterator entry(
        generated_root, fs::directory_options::none, error);
    const fs::recursive_directory_iterator end;
    if (error) {
        throw_index_error("snapshot-layout-unsafe",
                          "Cannot enumerate the generated snapshot directory.",
                          false);
    }
    std::size_t entries = 0;
    while (entry != end) {
        if (++entries > kMaxSnapshotFilesystemEntries) {
            throw CliError("snapshot-file-count-exceeded", "resource",
                           "Generated snapshot layout exceeds 2048 filesystem entries.",
                           1, false,
                           "Vacuum or rebuild the generated snapshot before comparing it.");
        }
        const fs::file_status status = entry->symlink_status(error);
        if (error || fs::is_symlink(status) ||
            (!fs::is_directory(status) && !fs::is_regular_file(status))) {
            throw_index_error(
                "snapshot-layout-unsafe",
                "Generated snapshot entries must be regular files or non-symlink directories.",
                false);
        }
        if (fs::is_regular_file(status)) {
            const std::string relative =
                entry->path().lexically_relative(out_root).generic_string();
            if (!generated_snapshot_path_valid(relative) ||
                !files.insert(relative).second) {
                throw_index_error(
                    "snapshot-layout-unsafe",
                    "Generated snapshot contains an invalid or duplicate filesystem path.",
                    false);
            }
        }
        entry.increment(error);
        if (error) {
            throw_index_error(
                "snapshot-layout-unsafe",
                "Cannot enumerate the complete generated snapshot directory.", false);
        }
    }
    return files;
}

}  // namespace

void verify_snapshot_manifest_file(const fs::path& out_root) {
    if (shin::generated_io::publication_in_progress(out_root)) {
        throw_index_error("publication-incomplete", "Generated snapshot publication requires writer recovery.");
    }
    for (const auto& relative : {fs::path("docs"), fs::path("docs/kb"), kGeneratedDir}) {
        std::error_code error;
        const auto status = fs::symlink_status(out_root / relative, error);
        if (error || fs::is_symlink(status) || !fs::is_directory(status))
            throw_index_error("snapshot-layout-unsafe", "Generated snapshot directories must not be symlinks.", false);
    }
    const fs::path generated_root = out_root / kGeneratedDir;
    const fs::path manifest = generated_root / "manifest.json";
    require_regular_snapshot_file(manifest, "Generated manifest");
    if (fs::file_size(manifest) > kMaxSnapshotManifestBytes) {
        throw CliError("snapshot-manifest-too-large", "resource",
                       "Generated snapshot manifest exceeds the 64 MiB limit.", 1, false,
                       "Rebuild or split the generated corpus before retrieval.");
    }
}

void verify_snapshot_filesystem(const fs::path& out_root, const std::set<std::string>& expected_files) {
    verify_snapshot_manifest_file(out_root);
    const fs::path generated_root = out_root / kGeneratedDir;
    if (enumerate_snapshot_files(out_root, generated_root) != expected_files) {
        throw_index_error("snapshot-integrity-bijection-failed",
                          "Generated filesystem entries and declared outputs do not form an exact bijection.", false);
    }
}

fs::path canonical_snapshot_root(const std::string& value,
                                 const std::string& option) {
    std::error_code error;
    const fs::path absolute = fs::absolute(value).lexically_normal();
    const fs::file_status status = fs::symlink_status(absolute, error);
    if (error || !fs::is_directory(status) || fs::is_symlink(status)) {
        throw_usage_error("invalid-snapshot-root",
                          option +
                              " must name an existing non-symlink output directory.");
    }
    const fs::path canonical = fs::canonical(absolute, error);
    if (error) {
        throw_usage_error("invalid-snapshot-root",
                          "Cannot canonicalize " + option + ".");
    }
    return canonical;
}

VerifiedSnapshot load_verified_snapshot(const fs::path& out_root,
                                        std::uintmax_t maximum_payload_bytes,
                                        std::uintmax_t maximum_file_bytes) {
    if (shin::generated_io::publication_in_progress(out_root)) {
        throw CliError(
            "publication-incomplete", "index",
            "Generated snapshot publication is incomplete: " + out_root.string(),
            1, true,
            "Wait for the publisher or run vacuum --apply before comparing snapshots.");
    }
    const fs::path generated_root = out_root / kGeneratedDir;
    std::error_code generated_error;
    const fs::file_status generated_status =
        fs::symlink_status(generated_root, generated_error);
    if (generated_error || fs::is_symlink(generated_status) ||
        !fs::is_directory(generated_status)) {
        throw_index_error(
            "snapshot-layout-unsafe",
            "Generated snapshot root must be a non-symlink directory.", false);
    }
    const fs::path manifest_path = generated_root / "manifest.json";
    require_regular_snapshot_file(manifest_path, "Generated manifest");
    std::error_code manifest_size_error;
    const std::uintmax_t manifest_bytes =
        fs::file_size(manifest_path, manifest_size_error);
    if (manifest_size_error || manifest_bytes > kMaxSnapshotManifestBytes) {
        throw CliError("snapshot-manifest-too-large", "resource",
                       "Generated snapshot manifest exceeds the 64 MiB limit.", 1,
                       false,
                       "Rebuild or split the generated corpus before comparing it.");
    }
    if (manifest_bytes > maximum_payload_bytes || manifest_bytes > maximum_file_bytes)
        throw CliError("snapshot-memory-budget-exceeded", "resource", "Snapshot exceeds its load budget.",
                       1, false, "Use a smaller snapshot or review the explicit load budget.");
    std::uintmax_t payload_bytes = manifest_bytes;
    const auto manifest_metadata = file_metadata(manifest_path);
    const std::string manifest_content = read_file(manifest_path);
    if (file_metadata(manifest_path) != manifest_metadata)
        throw_index_error("snapshot-integrity-failed", "Manifest changed during snapshot verification.", true);
    const Json manifest = parse_json_text(manifest_content, manifest_path.string());
    if (!manifest.is_object() ||
        json_string_value(manifest, "schema_version") !=
            "shin.context.kb-enterprise.v1" ||
        json_string_value(manifest, "tool") != "shin-context") {
        throw_index_error(
            "snapshot-manifest-invalid",
            "Generated snapshot manifest has an unsupported schema or tool.", false);
    }
    const Json* integrity =
        find_json_field(manifest, "generated_file_integrity");
    const Json* files =
        integrity == nullptr ? nullptr : find_json_field(*integrity, "files");
    if (integrity == nullptr || !integrity->is_object() ||
        json_string_value(*integrity, "algorithm") != "fnv1a64" ||
        files == nullptr || !files->is_array()) {
        throw_index_error(
            "snapshot-integrity-missing",
            "Generated snapshot manifest has no supported file integrity table.",
            false);
    }
    if (files->size() > kMaxSnapshotFiles) {
        throw CliError("snapshot-file-count-exceeded", "resource",
                       "Generated snapshot integrity table exceeds 1024 files.", 1,
                       false, "Review the generated snapshot before comparing it.");
    }

    VerifiedSnapshot snapshot;
    snapshot.out_root = out_root;
    snapshot.manifest_content = manifest_content;
    snapshot.manifest = manifest;
    snapshot.snapshot_id = fnv1a64(manifest_content);
    snapshot.schema_version = json_string_value(manifest, "schema_version");
    snapshot.profile = json_string_value(manifest, "profile");
    snapshot.fingerprints.emplace((kGeneratedDir / "manifest.json").generic_string(),
        FileFingerprint{manifest_metadata, snapshot.snapshot_id});
    snapshot.profile_config_hash =
        json_string_value(manifest, "profile_config_hash");
    snapshot.generator_version = json_string_value(manifest, "generator_version");
    const Json* row_counts = find_json_field(manifest, "row_counts");
    if (snapshot.profile.empty() || snapshot.profile_config_hash.empty() ||
        snapshot.generator_version.empty() || row_counts == nullptr ||
        !row_counts->is_object()) {
        throw_index_error(
            "snapshot-manifest-invalid",
            "Generated snapshot manifest is missing profile or row-count metadata.",
            false);
    }
    for (auto row = row_counts->begin(); row != row_counts->end(); ++row) {
        (void)required_nonnegative_size(*row_counts, row.key(), "row_counts");
    }
    snapshot.row_counts = *row_counts;
    snapshot.sources = load_manifest_entities(manifest, "sources");
    snapshot.source_facts =
        load_manifest_entities(manifest, "source_fact_sources");

    const Json* outputs = find_json_field(manifest, "outputs");
    if (outputs == nullptr || !outputs->is_array()) {
        throw_index_error("snapshot-manifest-invalid",
                          "Generated manifest outputs must be an array.", false);
    }
    if (outputs->size() > kMaxSnapshotFiles + 1) {
        throw CliError("snapshot-file-count-exceeded", "resource",
                       "Generated snapshot declares more than 1025 outputs.", 1,
                       false, "Review the generated snapshot before comparing it.");
    }
    std::set<std::string> expected_integrity_paths;
    bool manifest_output_seen = false;
    for (const Json& output : *outputs) {
        if (!output.is_object()) {
            throw_index_error("snapshot-manifest-invalid",
                              "Generated manifest output must be an object.", false);
        }
        const std::string path = json_string_value(output, "path");
        const std::string type = json_string_value(output, "type");
        (void)required_nonnegative_size(output, "rows", "outputs");
        if (!generated_snapshot_path_valid(path) ||
            (type != "json" && type != "jsonl" && type != "tsv" &&
            !(type == "shin.lexical.v1" && path == (kGeneratedDir / "terms.lex").generic_string() &&
              json_string_value(manifest, "term_index_format") == "shin.lexical.v1"))) {
            throw_index_error("snapshot-manifest-invalid",
                              "Generated manifest output is invalid.", false);
        }
        if (type == "shin.lexical.v1" && required_nonnegative_size(output, "rows", "outputs") !=
            required_nonnegative_size(*row_counts, "terms", "row_counts"))
            throw_index_error("snapshot-manifest-invalid", "Compact lexical row counts disagree.", false);
        if (path == (kGeneratedDir / "manifest.json").generic_string()) {
            if (manifest_output_seen || type != "json") {
                throw_index_error("snapshot-manifest-invalid",
                                  "Generated manifest output is duplicated.", false);
            }
            manifest_output_seen = true;
        } else if (!expected_integrity_paths.insert(path).second) {
            throw_index_error("snapshot-manifest-invalid",
                              "Generated output path is duplicated: " + path,
                              false);
        }
    }
    if (!manifest_output_seen) {
        throw_index_error("snapshot-manifest-invalid",
                          "Generated manifest does not declare itself.", false);
    }
    for (const Json& item : *files) {
        if (!item.is_object()) {
            throw_index_error(
                "snapshot-integrity-invalid",
                "Generated snapshot integrity entry must be an object.", false);
        }
        const std::string path = json_string_value(item, "path");
        const std::string expected_hash =
            json_string_value(item, "content_hash");
        const std::uintmax_t expected_bytes = required_nonnegative_size(
            item, "bytes", "generated_file_integrity.files");
        if (expected_bytes > maximum_file_bytes || expected_bytes > maximum_payload_bytes - payload_bytes)
            throw CliError("snapshot-memory-budget-exceeded", "resource", "Snapshot exceeds its load budget.",
                           1, false, "Use a smaller snapshot or review the explicit load budget.");
        payload_bytes += expected_bytes;
        if (!generated_snapshot_path_valid(path) || expected_hash.empty()) {
            throw_index_error(
                "snapshot-integrity-invalid",
                "Generated snapshot integrity entry has invalid path, bytes, or hash.",
                false);
        }
        const fs::path full_path = out_root / path;
        require_regular_snapshot_file(full_path, "Generated snapshot file");
        const FileFingerprint actual = fingerprint_file(full_path);
        if (actual.metadata.byte_count != expected_bytes ||
            actual.content_hash != expected_hash) {
            throw_index_error(
                "snapshot-integrity-failed",
                "Generated snapshot file does not match its manifest: " + path,
                false);
        }
        snapshot.fingerprints.emplace(path, actual);
        if (!snapshot.files
                 .emplace(path, SnapshotFile{path, actual.metadata.byte_count,
                                             actual.content_hash})
                 .second) {
            throw_index_error(
                "snapshot-integrity-invalid",
                "Generated snapshot integrity path is duplicated: " + path,
                false);
        }
    }
    std::set<std::string> actual_integrity_paths;
    for (const auto& [path, file] : snapshot.files) {
        (void)file;
        actual_integrity_paths.insert(path);
    }
    if (actual_integrity_paths != expected_integrity_paths) {
        throw_index_error(
            "snapshot-integrity-bijection-failed",
            "Generated outputs and integrity paths do not form an exact bijection.",
            false);
    }
    std::set<std::string> expected_files = expected_integrity_paths;
    expected_files.insert((kGeneratedDir / "manifest.json").generic_string());
    verify_snapshot_filesystem(out_root, expected_files);
    return snapshot;
}

Json verified_snapshot_description(const VerifiedSnapshot& snapshot) {
    return {
        {"out_root", snapshot.out_root.generic_string()},
        {"snapshot_id", snapshot.snapshot_id},
        {"schema_version", snapshot.schema_version},
        {"profile", snapshot.profile},
        {"profile_config_hash", snapshot.profile_config_hash},
        {"generator_version", snapshot.generator_version},
        {"integrity_status", "verified"},
        {"verified_file_count", snapshot.files.size()},
        {"row_counts", snapshot.row_counts}
    };
}

}  // namespace shin::context
