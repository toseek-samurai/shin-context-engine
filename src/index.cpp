#include "validation_generation.h"
#include "context_internal.h"
#include "compact_terms.h"
#include "generated_snapshot.h"
#include "generation_admission.h"
#include "generated_row_decoder.h"
#include "generated_io.h"
#include "snapshot_verification.h"

namespace shin::context {

bool compact_lexical_manifest(const Json& manifest) {
    const auto generator = json_string_value(manifest, "generator_version");
    const auto format = json_string_value(manifest, "term_index_format");
    if (generator == kGeneratorVersion && format == "shin.lexical.v1") return true;
    throw_index_error("index-generator-unsupported", "Unsupported lexical format or generator identity.", false);
}

bool compact_lexical_index(const RuntimeOptions& options) {
    if (options.generated_snapshot) return compact_lexical_manifest(options.generated_snapshot->manifest());
    const auto path = options.out_root / kGeneratedDir / "manifest.json";
    if (!fs::exists(path)) return true;
    verify_snapshot_manifest_file(options.out_root);
    return compact_lexical_manifest(parse_json_text(read_file(path), path.string()));
}

std::vector<std::string> expected_generated_relative_files(const RuntimeOptions& options) {
    (void)compact_lexical_index(options);
    return expected_generated_relative_files();
}

std::vector<std::string> expected_generated_relative_files() {
    std::vector<std::string> paths{
        (kGeneratedDir / "documents.jsonl").generic_string(),
        (kGeneratedDir / "document-rows.json").generic_string(),
        (kGeneratedDir / "sections.jsonl").generic_string(),
        (kGeneratedDir / "sections.index.tsv").generic_string(),
        (kGeneratedDir / "links.jsonl").generic_string(),
        (kGeneratedDir / "topics.jsonl").generic_string(),
        (kGeneratedDir / "decisions.jsonl").generic_string(),
        (kGeneratedDir / "decisions.index.tsv").generic_string(),
        (kGeneratedDir / "contracts.jsonl").generic_string(),
        (kGeneratedDir / "contracts.index.tsv").generic_string(),
        (kGeneratedDir / "runbooks.jsonl").generic_string(),
        (kGeneratedDir / "runbooks.index.tsv").generic_string(),
        (kGeneratedDir / "facts.jsonl").generic_string(),
        (kGeneratedDir / "facts.index.tsv").generic_string(),
        (kGeneratedDir / "manifest.json").generic_string()
    };
    paths.push_back((kGeneratedDir / "terms.lex").generic_string());
    return paths;
}

bool generated_index_available(const RuntimeOptions& options) {
    if (shin::generated_io::publication_in_progress(options.out_root)) {
        return false;
    }
    for (const std::string& relative : expected_generated_relative_files(options)) {
        if (!fs::exists(options.out_root / relative)) {
            return false;
        }
    }
    return true;
}

int generated_index_source_count(const RuntimeOptions& options) {
    const fs::path manifest_path = options.out_root / kGeneratedDir / "manifest.json";
    if (!fs::exists(manifest_path)) {
        throw_index_error("index-unavailable", "Generated KB manifest is missing: " + manifest_path.string());
    }
    const Json manifest = parse_json_text(read_file(manifest_path), manifest_path.string());
    return json_int_value(manifest, "source_count", 0);
}

IndexFreshness source_scan_freshness(const SourceScanCorpus& corpus) {
    IndexFreshness freshness;
    freshness.status = "source-scan";
    freshness.mode = "source-scan";
    freshness.source_count = static_cast<int>(corpus.documents.size());
    freshness.source_fact_candidate_files = corpus.source_facts.candidate_file_count;
    freshness.source_fact_scanned_files = corpus.source_facts.scanned_file_count;
    freshness.source_fact_skipped_large_files = corpus.source_facts.skipped_large_file_count;
    freshness.source_fact_capped_files = corpus.source_facts.capped_file_count;
    freshness.source_fact_rows = static_cast<int>(corpus.source_facts.rows.size());
    freshness.current = true;
    freshness.fully_verified = freshness.source_fact_skipped_large_files == 0 &&
                               freshness.source_fact_capped_files == 0;
    freshness.notes.push_back(
        "Generated index is not used; live documents and configured source facts were scanned.");
    if (!freshness.fully_verified) {
        freshness.notes.push_back(
            "Profile source-fact limits omitted configured inputs from the live scan.");
    }
    return freshness;
}

void require_complete_source_scan_or_throw(const IndexFreshness& freshness) {
    if (!freshness.current || !freshness.fully_verified) {
        throw_index_error(
            "incomplete-source-scan",
            "--require-current failed because configured source-fact inputs were skipped or capped.");
    }
}

static IndexFreshness assess_index_freshness_exact(const RuntimeOptions& options) {
    IndexFreshness freshness;
    freshness.mode = "full-generated-output";
    const auto expected_generation = validation_generated_output(options);
    const auto& generated = *expected_generation;
    freshness.source_count = generated.document_count;
    for (const auto& [path, expected] : generated.files) {
        const fs::path full_path = options.out_root / path;
        if (!fs::exists(full_path)) {
            ++freshness.missing_files;
            continue;
        }
        if (path != (kGeneratedDir / "manifest.json").generic_string() &&
            fs::file_size(full_path) != expected.size()) {
            ++freshness.stale_files;
            continue;
        }
        const std::string actual = read_file(full_path);
        if (path == (kGeneratedDir / "manifest.json").generic_string()) {
            freshness.snapshot_id = fnv1a64(actual);
        }
        if (actual != expected) {
            ++freshness.stale_files;
        }
    }
    if (generated.broken_local_link_count > 0) {
        if (options.profile.bootstrap_only) {
            freshness.notes.push_back(
                "Bootstrap output contains broken local links recorded as diagnostics.");
        } else {
            ++freshness.stale_files;
            freshness.notes.push_back("Generated output contains broken local links.");
        }
    }
    freshness.current = freshness.missing_files == 0 && freshness.stale_files == 0;
    freshness.fully_verified = freshness.current;
    freshness.status = freshness.current ? "current" : "stale";
    return freshness;
}

const std::string kFreshnessCacheSchema = "shin.freshness-cache.v1";

struct FreshnessCacheEntry {
    std::string scope;
    std::string path;
    std::string status;
    FileMetadata metadata;

    bool operator==(const FreshnessCacheEntry&) const = default;
};

static bool freshness_cache_entry_less(const FreshnessCacheEntry& left, const FreshnessCacheEntry& right) {
    return std::tie(left.scope, left.path, left.status) < std::tie(right.scope, right.path, right.status);
}

struct FreshnessCacheRecord {
    std::string generator_version;
    std::string manifest_hash;
    std::string profile;
    std::string root;
    std::string out_root;
    std::string verification;
    std::string profile_config_hash;
    int source_count = -1;
    int source_fact_candidate_files = -1;
    int source_fact_capped_files = -1;
    std::vector<FreshnessCacheEntry> entries;
};

struct CurrentFreshnessMetadata {
    std::vector<FreshnessCacheEntry> entries;
    int source_fact_candidate_files = 0;
    int source_fact_capped_files = 0;
};

static std::optional<int> cache_nonnegative_int(const Json& object, const std::string& key) {
    const Json* value = find_json_field(object, key);
    if (value == nullptr || !value->is_number_integer()) {
        return std::nullopt;
    }
    try {
        if (value->is_number_unsigned()) {
            const std::uint64_t parsed = value->get<std::uint64_t>();
            if (parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                return std::nullopt;
            }
            return static_cast<int>(parsed);
        }
        const std::int64_t parsed = value->get<std::int64_t>();
        if (parsed < 0 || parsed > std::numeric_limits<int>::max()) {
            return std::nullopt;
        }
        return static_cast<int>(parsed);
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

static std::optional<std::uintmax_t> cache_byte_count(const Json& value) {
    if (!value.is_number_integer()) {
        return std::nullopt;
    }
    try {
        if (value.is_number_unsigned()) {
            const std::uint64_t parsed = value.get<std::uint64_t>();
            if (parsed > std::numeric_limits<std::uintmax_t>::max()) {
                return std::nullopt;
            }
            return static_cast<std::uintmax_t>(parsed);
        }
        const std::int64_t parsed = value.get<std::int64_t>();
        return parsed < 0 ? std::nullopt
                          : std::optional<std::uintmax_t>(static_cast<std::uintmax_t>(parsed));
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

static std::optional<std::int64_t> cache_modified_ticks(const Json& value) {
    if (!value.is_number_integer()) {
        return std::nullopt;
    }
    try {
        if (value.is_number_unsigned()) {
            const std::uint64_t parsed = value.get<std::uint64_t>();
            if (parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return std::nullopt;
            }
            return static_cast<std::int64_t>(parsed);
        }
        return value.get<std::int64_t>();
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

static std::optional<FreshnessCacheRecord> load_freshness_cache(const RuntimeOptions& options) {
    const std::optional<std::string> content = shin::generated_io::read_freshness_cache(options.out_root);
    if (!content.has_value()) {
        return std::nullopt;
    }
    Json root;
    if (!try_parse_json_text(*content, root) || !root.is_object() ||
        json_string_value(root, "schema_version") != kFreshnessCacheSchema) {
        return std::nullopt;
    }
    const Json* files = find_json_field(root, "files");
    if (files == nullptr || !files->is_array()) {
        return std::nullopt;
    }

    FreshnessCacheRecord record;
    record.generator_version = json_string_value(root, "generator_version");
    record.manifest_hash = json_string_value(root, "manifest_hash");
    record.profile = json_string_value(root, "profile");
    record.root = json_string_value(root, "root");
    record.out_root = json_string_value(root, "out_root");
    record.verification = json_string_value(root, "verification");
    record.profile_config_hash = json_string_value(root, "profile_config_hash");
    const std::optional<int> source_count = cache_nonnegative_int(root, "source_count");
    const std::optional<int> source_fact_candidate_files =
        cache_nonnegative_int(root, "source_fact_candidate_files");
    const std::optional<int> source_fact_capped_files =
        cache_nonnegative_int(root, "source_fact_capped_files");
    if (record.profile_config_hash.empty() || !source_count.has_value() ||
        !source_fact_candidate_files.has_value() || !source_fact_capped_files.has_value()) {
        return std::nullopt;
    }
    record.source_count = *source_count;
    record.source_fact_candidate_files = *source_fact_candidate_files;
    record.source_fact_capped_files = *source_fact_capped_files;
    record.entries.reserve(files->size());
    for (const Json& item : *files) {
        if (!item.is_object() || !item.contains("bytes") || !item.contains("modified_ticks") ||
            !item.contains("changed_ticks") || !item.contains("device_id") || !item.contains("file_id")) {
            return std::nullopt;
        }
        FreshnessCacheEntry entry;
        entry.scope = json_string_value(item, "scope");
        entry.path = json_string_value(item, "path");
        entry.status = json_string_value(item, "status");
        if (entry.scope.empty() || entry.path.empty()) {
            return std::nullopt;
        }
        const std::optional<std::uintmax_t> byte_count = cache_byte_count(item.at("bytes"));
        const std::optional<std::int64_t> modified_ticks = cache_modified_ticks(item.at("modified_ticks"));
        const std::optional<std::int64_t> changed_ticks = cache_modified_ticks(item.at("changed_ticks"));
        const std::optional<std::uintmax_t> device_id = cache_byte_count(item.at("device_id"));
        const std::optional<std::uintmax_t> file_id = cache_byte_count(item.at("file_id"));
        if (!byte_count.has_value() || !modified_ticks.has_value() || !changed_ticks.has_value() ||
            !device_id.has_value() || *device_id > std::numeric_limits<std::uint64_t>::max() ||
            !file_id.has_value() || *file_id > std::numeric_limits<std::uint64_t>::max()) {
            return std::nullopt;
        }
        entry.metadata = {*byte_count, *modified_ticks, *changed_ticks,
                          static_cast<std::uint64_t>(*device_id), static_cast<std::uint64_t>(*file_id)};
        record.entries.push_back(std::move(entry));
    }
    std::sort(record.entries.begin(), record.entries.end(), freshness_cache_entry_less);
    if (std::adjacent_find(record.entries.begin(), record.entries.end(),
                           [](const FreshnessCacheEntry& left, const FreshnessCacheEntry& right) {
                               return left.scope == right.scope && left.path == right.path;
                           }) != record.entries.end()) {
        return std::nullopt;
    }
    return record;
}

static CurrentFreshnessMetadata current_freshness_metadata(
    const RuntimeOptions& options,
    const std::vector<std::string>& source_paths) {
    struct MetadataTask {
        std::string scope;
        std::string path;
        fs::path full_path;
        bool source_fact = false;
    };

    const SourceFactPathResult source_fact_paths = enumerate_source_fact_paths(options.profile, options.root);
    CurrentFreshnessMetadata current;
    current.source_fact_candidate_files = source_fact_paths.candidate_file_count;
    current.source_fact_capped_files = source_fact_paths.capped_file_count;

    std::vector<MetadataTask> tasks;
    tasks.reserve(source_paths.size() + source_fact_paths.paths.size() +
                  expected_generated_relative_files(options).size());
    for (const std::string& path : source_paths) {
        tasks.push_back({"document", path, options.root / path, false});
    }
    for (const std::string& path : source_fact_paths.paths) {
        tasks.push_back({"source-fact", path, options.root / path, true});
    }
    const std::string manifest_relative = (kGeneratedDir / "manifest.json").generic_string();
    for (const std::string& path : expected_generated_relative_files(options)) {
        if (path != manifest_relative) {
            tasks.push_back({"generated", path, options.out_root / path, false});
        }
    }

    current.entries.resize(tasks.size());
    parallel_for_indices(tasks.size(), [&](std::size_t index) {
        const MetadataTask& task = tasks[index];
        FreshnessCacheEntry& entry = current.entries[index];
        entry.scope = task.scope;
        entry.path = task.path;
        entry.metadata = file_metadata(task.full_path);
        if (task.source_fact) {
            entry.status = options.profile.source_fact_max_file_bytes > 0 &&
                                   entry.metadata.byte_count >
                                       static_cast<std::uintmax_t>(options.profile.source_fact_max_file_bytes)
                               ? "skipped-large"
                               : "scanned";
        } else {
            entry.status = task.scope == "generated" ? "published" : "indexed";
        }
    });
    std::sort(current.entries.begin(), current.entries.end(), freshness_cache_entry_less);
    return current;
}

static bool freshness_cache_matches(const RuntimeOptions& options,
                             const std::string& manifest_hash,
                             const std::string& current_profile_config_hash,
                             const std::vector<std::string>& source_paths) {
    const std::optional<FreshnessCacheRecord> cache = load_freshness_cache(options);
    if (!cache.has_value() || cache->generator_version != kGeneratorVersion ||
        cache->manifest_hash != manifest_hash || cache->profile != options.profile.id ||
        cache->root != options.root.generic_string() || cache->out_root != options.out_root.generic_string() ||
        cache->verification != "full-content-hash" ||
        cache->profile_config_hash != current_profile_config_hash ||
        cache->source_count != static_cast<int>(source_paths.size())) {
        return false;
    }
    const CurrentFreshnessMetadata current = current_freshness_metadata(options, source_paths);
    if (cache->source_fact_candidate_files != current.source_fact_candidate_files ||
        cache->source_fact_capped_files != current.source_fact_capped_files) {
        return false;
    }
    return cache->entries == current.entries;
}

static void persist_freshness_cache(const RuntimeOptions& options,
                             const std::string& manifest_hash,
                             const std::string& current_profile_config_hash,
                             int source_count,
                             int source_fact_candidate_files,
                             int source_fact_capped_files,
                             std::vector<FreshnessCacheEntry> entries,
                             IndexFreshness& freshness) {
    std::sort(entries.begin(), entries.end(), freshness_cache_entry_less);
    Json cache = {
        {"schema_version", kFreshnessCacheSchema},
        {"generator_version", kGeneratorVersion},
        {"manifest_hash", manifest_hash},
        {"profile", options.profile.id},
        {"root", options.root.generic_string()},
        {"out_root", options.out_root.generic_string()},
        {"verification", "full-content-hash"},
        {"profile_config_hash", current_profile_config_hash},
        {"source_count", source_count},
        {"source_fact_candidate_files", source_fact_candidate_files},
        {"source_fact_capped_files", source_fact_capped_files},
        {"files", Json::array()}
    };
    for (const FreshnessCacheEntry& entry : entries) {
        cache["files"].push_back({
            {"scope", entry.scope},
            {"path", entry.path},
            {"status", entry.status},
            {"bytes", entry.metadata.byte_count},
            {"modified_ticks", entry.metadata.modified_ticks},
            {"changed_ticks", entry.metadata.changed_ticks},
            {"device_id", entry.metadata.device_id},
            {"file_id", entry.metadata.file_id}
        });
    }
    try {
        shin::generated_io::write_freshness_cache(options.out_root, cache.dump());
    } catch (const std::exception&) {
        freshness.notes.push_back("Full verification passed, but the optional freshness cache was not updated.");
    }
}

static std::vector<SourceFactSource> current_source_fact_sources(const RuntimeOptions& options,
                                                          const SourceFactPathResult& paths) {
    std::vector<SourceFactSource> sources(paths.paths.size());
    parallel_for_indices(paths.paths.size(), [&](std::size_t index) {
        SourceFactSource& source = sources[index];
        source.path = paths.paths[index];
        const fs::path full_path = options.root / source.path;
        const FileMetadata metadata = file_metadata(full_path);
        if (options.profile.source_fact_max_file_bytes > 0 &&
            metadata.byte_count > static_cast<std::uintmax_t>(options.profile.source_fact_max_file_bytes)) {
            const FileMetadata after = file_metadata(full_path);
            if (metadata != after) {
                throw_file_changed_during_verification(full_path);
            }
            source.byte_count = metadata.byte_count;
            source.status = "skipped-large";
            source.metadata = metadata;
            source.has_metadata = true;
            return;
        }
        const FileFingerprint fingerprint = fingerprint_file(full_path);
        source.byte_count = fingerprint.metadata.byte_count;
        source.content_hash = fingerprint.content_hash;
        source.status = "scanned";
        source.metadata = fingerprint.metadata;
        source.has_metadata = true;
    });
    return sources;
}

static void assess_source_fact_freshness(const RuntimeOptions& options,
                                  const Json& manifest,
                                  IndexFreshness& freshness,
                                  std::vector<FreshnessCacheEntry>& cache_entries) {
    const SourceFactPathResult paths = enumerate_source_fact_paths(options.profile, options.root);
    const Json* manifest_counts = find_json_field(manifest, "source_fact_counts");
    if (manifest_counts == nullptr || !manifest_counts->is_object()) {
        ++freshness.stale_files;
        freshness.notes.push_back("Manifest source_fact_counts is missing.");
    } else if (json_int_value(*manifest_counts, "candidate_files", -1) != paths.candidate_file_count ||
               json_int_value(*manifest_counts, "capped_files", -1) != paths.capped_file_count) {
        ++freshness.stale_sources;
        freshness.notes.push_back("Source-fact candidate set changed.");
    }

    std::map<std::string, SourceFactSource> manifest_sources;
    const Json* source_array = find_json_field(manifest, "source_fact_sources");
    if (source_array == nullptr || !source_array->is_array()) {
        ++freshness.stale_files;
        freshness.notes.push_back("Manifest source_fact_sources is missing.");
        return;
    }
    for (const Json& item : *source_array) {
        if (!item.is_object()) {
            ++freshness.stale_files;
            continue;
        }
        SourceFactSource source;
        source.path = json_string_value(item, "path");
        source.status = json_string_value(item, "status");
        source.content_hash = json_string_value(item, "content_hash");
        source.byte_count = item.value("bytes", std::uintmax_t{0});
        if (source.path.empty() || !manifest_sources.emplace(source.path, source).second) {
            ++freshness.stale_files;
        }
    }

    const std::vector<SourceFactSource> current_sources = current_source_fact_sources(options, paths);
    std::set<std::string> current_paths;
    for (const SourceFactSource& current : current_sources) {
        if (current.has_metadata) {
            cache_entries.push_back({"source-fact", current.path, current.status, current.metadata});
        }
        current_paths.insert(current.path);
        const auto expected = manifest_sources.find(current.path);
        if (expected == manifest_sources.end()) {
            ++freshness.missing_sources;
            continue;
        }
        const SourceFactSource& recorded = expected->second;
        if (recorded.status != current.status || recorded.byte_count != current.byte_count ||
            recorded.content_hash != current.content_hash) {
            ++freshness.stale_sources;
        }
    }
    for (const auto& [path, source] : manifest_sources) {
        (void)source;
        if (current_paths.find(path) == current_paths.end()) {
            ++freshness.extra_sources;
        }
    }
}

static void assess_generated_file_integrity(const RuntimeOptions& options,
                                     const Json& manifest,
                                     IndexFreshness& freshness,
                                     std::vector<FreshnessCacheEntry>& cache_entries) {
    const int initial_stale_files = freshness.stale_files;
    const Json* integrity = find_json_field(manifest, "generated_file_integrity");
    if (integrity == nullptr || !integrity->is_object()) {
        ++freshness.stale_files;
        freshness.notes.push_back("Manifest generated_file_integrity is missing.");
        return;
    }
    if (json_string_value(*integrity, "algorithm") != "fnv1a64") {
        ++freshness.stale_files;
        freshness.notes.push_back("Manifest generated-file integrity algorithm is unsupported.");
    }

    const Json* files = find_json_field(*integrity, "files");
    if (files == nullptr || !files->is_array()) {
        ++freshness.stale_files;
        freshness.notes.push_back("Manifest generated-file integrity list is missing.");
        return;
    }

    struct RecordedFile {
        std::uintmax_t byte_count = 0;
        std::string content_hash;
    };
    std::map<std::string, RecordedFile> recorded_files;
    for (const Json& item : *files) {
        if (!item.is_object()) {
            ++freshness.stale_files;
            continue;
        }
        const std::string path = json_string_value(item, "path");
        const std::string content_hash = json_string_value(item, "content_hash");
        if (path.empty() || content_hash.empty() ||
            !recorded_files.emplace(path, RecordedFile{item.value("bytes", std::uintmax_t{0}), content_hash}).second) {
            ++freshness.stale_files;
        }
    }

    std::vector<std::string> expected_paths = expected_generated_relative_files(options);
    expected_paths.erase(std::remove(expected_paths.begin(), expected_paths.end(),
                                     (kGeneratedDir / "manifest.json").generic_string()),
                         expected_paths.end());
    std::vector<FileFingerprint> current_fingerprints(expected_paths.size());
    std::vector<unsigned char> current_status(expected_paths.size(), 0);
    parallel_for_indices(expected_paths.size(), [&](std::size_t index) {
        const fs::path path = options.out_root / expected_paths[index];
        if (fs::exists(path) && fs::is_regular_file(path)) {
            current_fingerprints[index] = fingerprint_file(path);
            current_status[index] = 1;
        }
    });

    std::set<std::string> expected_set;
    for (std::size_t index = 0; index < expected_paths.size(); ++index) {
        const std::string& path = expected_paths[index];
        expected_set.insert(path);
        if (current_status[index] == 0) {
            ++freshness.missing_files;
            continue;
        }
        cache_entries.push_back({"generated", path, "published", current_fingerprints[index].metadata});
        const auto recorded = recorded_files.find(path);
        if (recorded == recorded_files.end()) {
            ++freshness.stale_files;
            continue;
        }
        const FileFingerprint& current = current_fingerprints[index];
        if (recorded->second.byte_count != current.metadata.byte_count ||
            recorded->second.content_hash != current.content_hash) {
            ++freshness.stale_files;
        }
    }
    for (const auto& [path, recorded] : recorded_files) {
        (void)recorded;
        if (expected_set.find(path) == expected_set.end()) {
            ++freshness.stale_files;
        }
    }
    if (freshness.stale_files > initial_stale_files) {
        freshness.notes.push_back("Generated-file integrity metadata does not match the published snapshot.");
    }
}

static IndexFreshness assess_index_freshness_fast(const RuntimeOptions& options, bool publish_cache = false,
                                                 bool use_metadata_cache = true) {
    IndexFreshness freshness;
    freshness.mode = "manifest-all-source-and-output-hash";
    for (const std::string& relative : expected_generated_relative_files(options)) {
        if (!fs::exists(options.out_root / relative)) {
            ++freshness.missing_files;
        }
    }
    const fs::path manifest_path = options.out_root / kGeneratedDir / "manifest.json";
    if (freshness.missing_files > 0 || !fs::exists(manifest_path)) {
        freshness.status = "missing";
        freshness.current = false;
        freshness.fully_verified = false;
        return freshness;
    }

    const std::string manifest_content = read_file(manifest_path);
    const std::string manifest_hash = fnv1a64(manifest_content);
    freshness.snapshot_id = manifest_hash;
    const std::string current_profile_config_hash = profile_config_hash(options.profile);
    const std::vector<std::string> source_paths = enumerate_source_paths(options.profile, options.root);
    if (use_metadata_cache && freshness_cache_matches(options, manifest_hash, current_profile_config_hash, source_paths)) {
        freshness.status = "current";
        freshness.mode = "verified-metadata-cache";
        freshness.current = true;
        freshness.fully_verified = true;
        freshness.cache_hit = true;
        freshness.source_count = static_cast<int>(source_paths.size());
        return freshness;
    }

    const Json manifest = parse_json_text(manifest_content, manifest_path.string());
    if (json_string_value(manifest, "generator_version") != kGeneratorVersion) {
        ++freshness.stale_files;
        freshness.notes.push_back("Manifest generator_version does not match the current generator.");
    }
    if (json_string_value(manifest, "profile") != options.profile.id) {
        ++freshness.stale_files;
        freshness.notes.push_back("Manifest profile does not match the requested profile.");
    }
    const std::string manifest_profile_config_hash = json_string_value(manifest, "profile_config_hash");
    if (manifest_profile_config_hash.empty() ||
        manifest_profile_config_hash != current_profile_config_hash) {
        ++freshness.stale_files;
        freshness.notes.push_back("Profile config hash is missing or stale.");
    }

    const int manifest_source_count = json_int_value(manifest, "source_count", -1);
    freshness.source_count = std::max(0, manifest_source_count);
    if (manifest_source_count != static_cast<int>(source_paths.size())) {
        ++freshness.stale_files;
        freshness.notes.push_back("Manifest source_count does not match the current source set.");
    }

    std::vector<FreshnessCacheEntry> cache_entries;
    cache_entries.reserve(source_paths.size() + expected_generated_relative_files(options).size());
    std::vector<FileFingerprint> source_fingerprints(source_paths.size());
    std::vector<unsigned char> source_status(source_paths.size(), 0);
    parallel_for_indices(source_paths.size(), [&](std::size_t index) {
        const fs::path full_path = options.root / source_paths[index];
        if (fs::exists(full_path) && fs::is_regular_file(full_path)) {
            source_fingerprints[index] = fingerprint_file(full_path);
            source_status[index] = 1;
        }
    });

    std::map<std::string, std::string> expected_hashes;
    for (std::size_t index = 0; index < source_paths.size(); ++index) {
        if (source_status[index] == 0) {
            ++freshness.missing_sources;
            continue;
        }
        const FileFingerprint& fingerprint = source_fingerprints[index];
        expected_hashes[source_paths[index]] = fingerprint.content_hash;
        cache_entries.push_back({"document", source_paths[index], "indexed", fingerprint.metadata});
    }

    std::set<std::string> manifest_paths;
    const Json* sources = find_json_field(manifest, "sources");
    if (sources == nullptr || !sources->is_array()) {
        ++freshness.stale_files;
        freshness.notes.push_back("Manifest does not contain a sources array.");
    } else {
        for (const Json& source : *sources) {
            if (!source.is_object()) {
                ++freshness.stale_files;
                continue;
            }
            const std::string path = json_string_value(source, "path");
            if (path.empty()) {
                ++freshness.stale_files;
                continue;
            }
            if (!manifest_paths.insert(path).second) {
                ++freshness.stale_files;
                continue;
            }
            auto expected = expected_hashes.find(path);
            if (expected == expected_hashes.end()) {
                ++freshness.extra_sources;
                continue;
            }
            if (json_string_value(source, "content_hash") != expected->second) {
                ++freshness.stale_sources;
            }
        }
    }
    for (const auto& [path, hash] : expected_hashes) {
        (void)hash;
        if (manifest_paths.find(path) == manifest_paths.end()) {
            ++freshness.missing_sources;
        }
    }

    assess_generated_file_integrity(options, manifest, freshness, cache_entries);
    assess_source_fact_freshness(options, manifest, freshness, cache_entries);

    freshness.current = freshness.missing_files == 0 && freshness.stale_files == 0 &&
                        freshness.missing_sources == 0 && freshness.stale_sources == 0 &&
                        freshness.extra_sources == 0;
    freshness.fully_verified = freshness.current;
    freshness.status = freshness.current ? "current" : "stale";
    if (freshness.current && publish_cache) {
        const Json& source_fact_counts = manifest.at("source_fact_counts");
        persist_freshness_cache(options,
                                manifest_hash,
                                current_profile_config_hash,
                                manifest_source_count,
                                json_int_value(source_fact_counts, "candidate_files", -1),
                                json_int_value(source_fact_counts, "capped_files", -1),
                                std::move(cache_entries),
                                freshness);
    }
    return freshness;
}

IndexFreshness assess_index_freshness(const RuntimeOptions& options, bool require_current) {
    if (require_current && options.generation_admission)
        return options.generation_admission->assess(options);
    const auto paths = expected_generated_relative_files(options);
    verify_snapshot_filesystem(options.out_root, {paths.begin(), paths.end()});
    return require_current ? assess_index_freshness_exact(options) : assess_index_freshness_fast(options);
}

IndexFreshness assess_source_discovery_freshness(const RuntimeOptions& options) {
    const auto paths = expected_generated_relative_files(options);
    verify_snapshot_filesystem(options.out_root, {paths.begin(), paths.end()});
    // Source discovery reads current source bytes, not generated fact bodies.
    // Verify the entire inventory and content hashes without regenerating facts.
    return assess_index_freshness_fast(options, false, false);
}

void prepare_freshness_cache(const RuntimeOptions& options) {
    const auto lock = lock_index_for_read(options.out_root);
    const auto paths = expected_generated_relative_files(options);
    verify_snapshot_filesystem(options.out_root, {paths.begin(), paths.end()});
    (void)assess_index_freshness_fast(options, true);
}

void require_current_index_or_throw(const IndexFreshness& freshness) {
    if (!freshness.current || !freshness.fully_verified) {
        throw_index_error("stale-index",
                          "--require-current failed because the generated index is not fully current.");
    }
}

Json index_freshness_json(const IndexFreshness& freshness) {
    Json result = {
        {"status", freshness.status},
        {"mode", freshness.mode},
        {"current", freshness.current},
        {"fully_verified", freshness.fully_verified},
        {"cache_hit", freshness.cache_hit},
        {"missing_files", freshness.missing_files},
        {"stale_files", freshness.stale_files},
        {"missing_sources", freshness.missing_sources},
        {"stale_sources", freshness.stale_sources},
        {"extra_sources", freshness.extra_sources},
        {"notes", freshness.notes}
    };
    result["snapshot_id"] = freshness.snapshot_id.empty() ? Json(nullptr) : Json(freshness.snapshot_id);
    if (freshness.mode == "source-scan") {
        result["source_scan"] = {
            {"document_sources", freshness.source_count},
            {"source_fact_candidate_files", freshness.source_fact_candidate_files},
            {"source_fact_scanned_files", freshness.source_fact_scanned_files},
            {"source_fact_skipped_large_files", freshness.source_fact_skipped_large_files},
            {"source_fact_capped_files", freshness.source_fact_capped_files},
            {"source_fact_rows", freshness.source_fact_rows}
        };
    }
    return result;
}

void emit_index_freshness_json(std::ostream& out, const IndexFreshness& freshness) {
    out << index_freshness_json(freshness).dump();
}

bool should_use_generated_index(const RuntimeOptions& options) {
    if (options.force_source_scan) return false;
    if (options.force_index || shin::generated_io::publication_in_progress(options.out_root)) return true;
    std::error_code error;
    const auto status = fs::symlink_status(options.out_root / kGeneratedDir, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw_index_error("snapshot-layout-unsafe", "Cannot inspect the generated snapshot directory.", false);
    }
    // A partial publication is an index failure, not an implicit live-source fallback.
    return fs::exists(status);
}

constexpr long long kMaxGeneratedIndexRowBytes = 64LL * 1024LL * 1024LL;
constexpr std::size_t kMaxGeneratedOffsetIndexLineBytes = 1024U * 1024U;

static void append_bounded_line_bytes(std::string& line,
                                      const char* data,
                                      std::size_t bytes,
                                      std::size_t maximum_bytes,
                                      std::string_view label) {
    if (line.size() > maximum_bytes || bytes > maximum_bytes - line.size()) {
        throw_index_error("index-corrupt", std::string(label) + " exceeds its byte limit.");
    }
    line.append(data, bytes);
}

static bool read_bounded_generated_line(std::istream& input,
                                        std::string& line,
                                        std::size_t maximum_bytes,
                                        std::string_view label) {
    std::array<char, 8192> chunk{};
    line.clear();
    while (true) {
        const std::streamsize count =
            input.rdbuf()->sgetn(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        if (count <= 0) {
            return !line.empty();
        }
        const auto begin = chunk.begin();
        const auto end = begin + count;
        const auto newline = std::find(begin, end, '\n');
        const std::size_t append_bytes = static_cast<std::size_t>(newline - begin);
        append_bounded_line_bytes(line, chunk.data(), append_bytes, maximum_bytes, label);
        if (newline == end) {
            continue;
        }

        const std::streamoff unread = static_cast<std::streamoff>(end - newline - 1);
        if (unread > 0 &&
            input.rdbuf()->pubseekoff(-unread, std::ios_base::cur, std::ios_base::in) ==
                std::streampos(std::streamoff(-1))) {
            throw std::runtime_error("Cannot restore generated index stream position.");
        }
        return true;
    }
}

class BoundedGeneratedLineReader {
public:
    BoundedGeneratedLineReader(std::istream& input,
                               std::size_t maximum_bytes,
                               std::string_view label)
        : input_(input), maximum_bytes_(maximum_bytes), label_(label) {}

    bool read(std::string& line) {
        line.clear();
        while (true) {
            if (begin_ < end_) {
                const auto first = buffer_.begin() + static_cast<std::ptrdiff_t>(begin_);
                const auto last = buffer_.begin() + static_cast<std::ptrdiff_t>(end_);
                const auto newline = std::find(first, last, '\n');
                const std::size_t bytes = static_cast<std::size_t>(newline - first);
                append_bounded_line_bytes(line, &buffer_[begin_], bytes, maximum_bytes_, label_);
                begin_ += bytes;
                if (newline != last) {
                    ++begin_;
                    return true;
                }
            }
            if (eof_) {
                return !line.empty();
            }

            input_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
            const std::streamsize count = input_.gcount();
            if (input_.bad()) {
                throw std::runtime_error("Cannot read generated index stream.");
            }
            begin_ = 0;
            end_ = count > 0 ? static_cast<std::size_t>(count) : 0;
            eof_ = count < static_cast<std::streamsize>(buffer_.size());
            if (end_ == 0 && eof_) {
                return !line.empty();
            }
        }
    }

private:
    std::istream& input_;
    std::size_t maximum_bytes_;
    std::string label_;
    std::array<char, 8192> buffer_{};
    std::size_t begin_ = 0;
    std::size_t end_ = 0;
    bool eof_ = false;
};

void for_each_jsonl_line_if_exists(const fs::path& path, const std::function<void(const std::string&)>& handle) {
    if (!fs::exists(path)) {
        return;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read " + path.string());
    }
    BoundedGeneratedLineReader reader(
        input, static_cast<std::size_t>(kMaxGeneratedIndexRowBytes), "Generated JSONL row");
    std::string line;
    while (reader.read(line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string compact = trim(line);
        if (!compact.empty()) {
            handle(compact);
        }
    }
}

static void for_each_snapshot_line(const RuntimeOptions& options, const fs::path& path,
                                   const std::function<void(const std::string&)>& handle) {
    if (!generated_file_exists(options, path)) return;
    GeneratedInput input(options, path);
    if (!input) throw_index_error("index-corrupt", "Cannot open generated JSONL input.");
    BoundedGeneratedLineReader reader(input, static_cast<std::size_t>(kMaxGeneratedIndexRowBytes), "Generated JSONL row");
    std::string line;
    while (reader.read(line)) {
        const auto compact = trim(line);
        if (!compact.empty()) handle(compact);
    }
}

SectionRow parse_section_row(const std::string& line) {
    return decode_generated_section_row(line);
}

SourceDoc parse_document_row(const std::string& line) {
    return decode_generated_document_row(line);
}

LinkRow parse_link_row(const std::string& line) {
    return decode_generated_link_row(line);
}

FactRow parse_generated_fact_row(const std::string& line) {
    return decode_generated_fact_row(line);
}

FactRow parse_decision_as_fact_row(const std::string& line) {
    return decode_generated_decision_row(line);
}

FactRow parse_typed_reference_as_fact_row(const std::string& line) {
    return decode_generated_typed_reference_row(line);
}

static void add_candidate_ids(std::set<int>& target, const std::vector<int>& ids) {
    for (int id : ids) {
        if (id >= 0) {
            target.insert(id);
        }
    }
}

static void merge_term_posting(CandidateRows& candidates, const TermPosting& posting) {
    add_candidate_ids(candidates.sections, posting.sections);
    add_candidate_ids(candidates.facts, posting.facts);
    add_candidate_ids(candidates.decisions, posting.decisions);
    add_candidate_ids(candidates.contracts, posting.contracts);
    add_candidate_ids(candidates.runbooks, posting.runbooks);
}

static void add_prepared_lookup_term(std::set<std::string>& terms, const std::string& raw) {
    const PreparedMatchTerm prepared = prepare_match_term(raw);
    auto add = [&](const std::string& value) {
        const std::string term = trim(to_lower(value));
        if (index_term_has_signal(term)) {
            terms.insert(term);
        }
    };
    add(prepared.raw);
    add(prepared.normalized);
    add(prepared.compact);
}

static std::set<std::string> term_lookup_set(const PreparedTermGroups& term_groups) {
    std::set<std::string> terms;
    for (const PreparedTermGroup& group : term_groups) {
        for (const PreparedMatchTerm& term : group) {
            add_prepared_lookup_term(terms, term.raw);
            add_prepared_lookup_term(terms, term.normalized);
            add_prepared_lookup_term(terms, term.compact);
            terms.insert(term.phrase_keys.begin(), term.phrase_keys.end());
        }
    }
    return terms;
}

static std::set<std::string> term_lookup_set(const PreparedTermGroup& term_group) {
    std::set<std::string> terms;
    for (const PreparedMatchTerm& term : term_group) {
        add_prepared_lookup_term(terms, term.raw);
        add_prepared_lookup_term(terms, term.normalized);
        add_prepared_lookup_term(terms, term.compact);
    }
    return terms;
}

static std::size_t candidate_row_count(const CandidateRows& candidates) {
    return candidates.sections.size() + candidates.facts.size() + candidates.decisions.size() +
           candidates.contracts.size() + candidates.runbooks.size();
}

static void apply_term_candidate_cutoff(CandidateRows& candidates, std::size_t max_rows) {
    const std::size_t row_count = candidate_row_count(candidates);
    candidates.discovered_row_count = row_count;
    candidates.from_terms_index = candidates.term_index_coverage_complete && row_count <= max_rows;
    if (!candidates.from_terms_index) {
        candidates.sections.clear();
        candidates.facts.clear();
        candidates.decisions.clear();
        candidates.contracts.clear();
        candidates.runbooks.clear();
    }
}

template <typename Integer>
bool parse_decimal_integer(std::string_view text, Integer& value) noexcept {
    if (text.empty()) {
        return false;
    }
    Integer parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [next, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || next != end) {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_row_offset_line(const std::string& line, RowOffset& out) {
    const std::string_view view(line);
    const std::size_t first_tab = view.find('\t');
    if (first_tab == std::string::npos) {
        return false;
    }
    const std::size_t second_tab = view.find('\t', first_tab + 1);
    if (second_tab == std::string::npos) {
        return false;
    }
    if (!parse_decimal_integer(view.substr(0, first_tab), out.row) ||
        !parse_decimal_integer(view.substr(first_tab + 1, second_tab - first_tab - 1), out.offset) ||
        !parse_decimal_integer(view.substr(second_tab + 1), out.bytes)) {
        return false;
    }
    return out.row >= 0 && out.offset >= 0 && out.bytes > 0;
}

static long long data_file_size(std::istream& input, const fs::path& path) {
    input.clear();
    input.seekg(0, std::ios::end);
    const std::streampos end = input.tellg();
    if (end == std::streampos(-1) || end > std::numeric_limits<long long>::max()) {
        throw_index_error("index-corrupt", "Cannot determine generated data size: " + path.string());
    }
    return static_cast<long long>(end);
}

static void require_valid_data_range(long long offset,
                                     long long bytes,
                                     long long file_bytes,
                                     const std::string& label) {
    if (offset < 0 || bytes <= 0 || bytes > kMaxGeneratedIndexRowBytes ||
        offset > file_bytes || bytes > file_bytes - offset) {
        throw_index_error("index-corrupt", label + " points outside its bounded data file.");
    }
}

struct OffsetIndexLine {
    long long offset = 0;
    long long next_offset = 0;
    std::string text;
};

static bool read_offset_index_line(std::istream& input,
                                   long long file_bytes,
                                   long long probe,
                                   OffsetIndexLine& out) {
    if (file_bytes <= 0 || probe < 0 || probe >= file_bytes) {
        return false;
    }
    long long line_offset = probe;
    std::array<char, 8192> reverse_chunk{};
    long long search_end = probe;
    bool line_start_found = false;
    while (search_end > 0 && !line_start_found) {
        const long long search_start =
            std::max(0LL, search_end - static_cast<long long>(reverse_chunk.size()));
        const std::streamsize search_bytes =
            static_cast<std::streamsize>(search_end - search_start);
        input.clear();
        input.seekg(search_start);
        input.read(reverse_chunk.data(), search_bytes);
        if (input.gcount() != search_bytes) {
            throw std::runtime_error("Cannot seek generated term offset index.");
        }
        for (std::streamsize index = search_bytes; index > 0; --index) {
            if (reverse_chunk[static_cast<std::size_t>(index - 1)] == '\n') {
                line_offset = search_start + index;
                line_start_found = true;
                break;
            }
        }
        search_end = search_start;
        if (!line_start_found &&
            probe - search_end > static_cast<long long>(kMaxGeneratedOffsetIndexLineBytes)) {
            throw_index_error("index-corrupt",
                              "Generated term offset index row exceeds its byte limit.");
        }
    }
    if (!line_start_found) {
        line_offset = 0;
    }

    input.clear();
    input.seekg(line_offset);
    if (!read_bounded_generated_line(input, out.text, kMaxGeneratedOffsetIndexLineBytes,
                                     "Generated term offset index row")) {
        return false;
    }
    const std::streampos next = input.tellg();
    out.offset = line_offset;
    out.next_offset = next == std::streampos(-1) ? file_bytes : static_cast<long long>(next);
    return out.next_offset > out.offset;
}

using TermPostingLookup = std::map<std::string, TermPosting>;

struct LoadedTermPostings {
    TermPostingLookup lookup;
    bool coverage_complete = true;
};

static void append_posting_rows(std::vector<int>& target, const std::vector<int>& source) {
    target.insert(target.end(), source.begin(), source.end());
}

static void append_term_posting(TermPosting& target, const TermPosting& source) {
    append_posting_rows(target.sections, source.sections);
    append_posting_rows(target.facts, source.facts);
    append_posting_rows(target.decisions, source.decisions);
    append_posting_rows(target.contracts, source.contracts);
    append_posting_rows(target.runbooks, source.runbooks);
}

static void normalize_posting_rows(std::vector<int>& rows) {
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
}

static void normalize_term_posting(TermPosting& posting) {
    normalize_posting_rows(posting.sections);
    normalize_posting_rows(posting.facts);
    normalize_posting_rows(posting.decisions);
    normalize_posting_rows(posting.contracts);
    normalize_posting_rows(posting.runbooks);
}

static LoadedTermPostings load_compact_term_postings(
    const RuntimeOptions& options, const std::set<std::string>& wanted, const std::set<std::string>& prefixes) {
    LoadedTermPostings loaded;
    if (wanted.empty()) return loaded;
    const auto manifest_path = options.out_root / kGeneratedDir / "manifest.json";
    Json disk_manifest;
    if (!options.generated_snapshot) disk_manifest = parse_json_text(read_file(manifest_path), manifest_path.string());
    const auto& manifest = options.generated_snapshot ? options.generated_snapshot->manifest() : disk_manifest;
    GeneratedInput input(options, options.out_root / kGeneratedDir / "terms.lex");
    CompactTerms terms(input, term_row_counts(manifest));
    if (terms.size() != compact_term_count(manifest))
        throw_index_error("index-corrupt", "Compact lexical count disagrees with the manifest.");
    std::map<std::string, std::uint32_t> selected;
    const auto add = [&](const std::string& key, std::uint32_t id) {
        selected.emplace(key, id);
        return prefixes.empty() || selected.size() <= 2048;
    };
    for (const auto& prefix : prefixes) {
        for (auto id = terms.lower_bound(prefix); id < terms.size(); ++id) {
            auto key = terms.term(id);
            if (!starts_with(key, prefix)) break;
            if (can_prefix_complete_lookup_term(key) && !add(key, id))
                return {{}, false};
        }
    }
    for (const auto& key : wanted) {
        const auto id = terms.lower_bound(key);
        if (id < terms.size() && terms.term(id) == key && !add(key, id)) return {{}, false};
    }
    std::size_t decoded_bytes = 0;
    for (const auto& [key, id] : selected) {
        auto posting = terms.posting(id);
        // Keep the existing prefix completion budget in legacy logical bytes,
        // so compression does not silently expand the candidate working set.
        if (!prefixes.empty()) {
            const auto logical = "{\"term\":" + json_string(key) +
                ",\"sections\":" + json_int_array(posting.sections) +
                ",\"facts\":" + json_int_array(posting.facts) +
                ",\"decisions\":" + json_int_array(posting.decisions) +
                ",\"contracts\":" + json_int_array(posting.contracts) +
                ",\"runbooks\":" + json_int_array(posting.runbooks) + "}\n";
            if (logical.size() > 64ULL * 1024ULL * 1024ULL - decoded_bytes) return {{}, false};
            decoded_bytes += logical.size();
        }
        if (wanted.contains(key)) append_term_posting(loaded.lookup[key], posting);
        for (const auto& prefix : prefixes)
            if (starts_with(key, prefix)) append_term_posting(loaded.lookup[prefix], posting);
    }
    for (auto& [key, posting] : loaded.lookup) { (void)key; normalize_term_posting(posting); }
    return loaded;
}

static CandidateRows candidates_for_term_group(const PreparedTermGroup& term_group,
                                        const TermPostingLookup& postings) {
    CandidateRows candidates;
    for (const std::string& term : term_lookup_set(term_group)) {
        const auto posting = postings.find(term);
        if (posting != postings.end()) {
            merge_term_posting(candidates, posting->second);
        }
    }
    constexpr std::array candidate_members{&CandidateRows::sections, &CandidateRows::facts,
        &CandidateRows::decisions, &CandidateRows::contracts, &CandidateRows::runbooks};
    constexpr std::array posting_members{&TermPosting::sections, &TermPosting::facts,
        &TermPosting::decisions, &TermPosting::contracts, &TermPosting::runbooks};
    for (const auto& term : term_group) {
        if (term.phrase_keys.empty()) continue;
        CandidateRows phrase;
        bool first = true;
        for (const auto& word : term.phrase_keys) {
            const auto posting = postings.find(word);
            if (posting == postings.end()) {
                phrase = {};
                break;
            }
            if (first) {
                merge_term_posting(phrase, posting->second);
                first = false;
            } else {
                for (std::size_t store = 0; store < candidate_members.size(); ++store) {
                    const auto& ids = posting->second.*posting_members[store];
                    std::erase_if(phrase.*candidate_members[store], [&](int id) {
                        return !std::binary_search(ids.begin(), ids.end(), id);
                    });
                }
            }
        }
        for (const auto member : candidate_members) {
            (candidates.*member).insert((phrase.*member).begin(), (phrase.*member).end());
        }
    }
    return candidates;
}

static std::set<std::string> prefix_lookup_set(const PreparedTermGroups& term_groups) {
    std::set<std::string> prefixes;
    if (term_groups.size() < static_cast<std::size_t>(kCompoundQueryCoverageThreshold)) {
        return prefixes;
    }
    for (const PreparedTermGroup& group : term_groups) {
        for (const PreparedMatchTerm& term : group) {
            if (term.allow_prefix && can_prefix_complete_lookup_term(term.compact)) {
                prefixes.insert(term.compact);
            }
        }
    }
    return prefixes;
}

using TermPostingRefs = std::vector<const TermPosting*>;

static TermPostingRefs posting_refs_for_term_group(const PreparedTermGroup& term_group,
                                                   const TermPostingLookup& postings) {
    TermPostingRefs refs;
    for (const std::string& term : term_lookup_set(term_group)) {
        const auto posting = postings.find(term);
        if (posting != postings.end()) {
            refs.push_back(&posting->second);
        }
    }
    return refs;
}

static std::size_t posting_ref_row_count(const TermPostingRefs& refs) {
    std::size_t count = 0;
    for (const TermPosting* posting : refs) {
        count += posting->sections.size() + posting->facts.size() + posting->decisions.size() +
                 posting->contracts.size() + posting->runbooks.size();
    }
    return count;
}

static bool posting_refs_contain_row(const TermPostingRefs& refs,
                                     const std::vector<int> TermPosting::*member,
                                     int row) {
    return std::any_of(refs.begin(), refs.end(), [&](const TermPosting* posting) {
        const std::vector<int>& rows = posting->*member;
        return std::binary_search(rows.begin(), rows.end(), row);
    });
}

static void retain_rows_matching_group(std::set<int>& rows,
                                       const TermPostingRefs& refs,
                                       const std::vector<int> TermPosting::*member) {
    for (auto row = rows.begin(); row != rows.end();) {
        if (posting_refs_contain_row(refs, member, *row)) {
            ++row;
        } else {
            row = rows.erase(row);
        }
    }
}

static CandidateRows select_candidates_matching_every_group(
    const std::vector<TermPostingRefs>& group_postings) {
    const auto seed = std::min_element(
        group_postings.begin(), group_postings.end(),
        [](const TermPostingRefs& left, const TermPostingRefs& right) {
            return posting_ref_row_count(left) < posting_ref_row_count(right);
        });
    CandidateRows candidates;
    for (const TermPosting* posting : *seed) {
        merge_term_posting(candidates, *posting);
    }
    for (const TermPostingRefs& refs : group_postings) {
        if (&refs == &*seed) {
            continue;
        }
        retain_rows_matching_group(candidates.sections, refs, &TermPosting::sections);
        retain_rows_matching_group(candidates.facts, refs, &TermPosting::facts);
        retain_rows_matching_group(candidates.decisions, refs, &TermPosting::decisions);
        retain_rows_matching_group(candidates.contracts, refs, &TermPosting::contracts);
        retain_rows_matching_group(candidates.runbooks, refs, &TermPosting::runbooks);
    }
    return candidates;
}

static void select_candidate_ids_by_group(std::set<int>& target,
                                   const std::vector<CandidateRows>& group_candidates,
                                   const std::set<int> CandidateRows::*member,
                                   int required_group_matches) {
    std::unordered_map<int, int> match_counts;
    for (const CandidateRows& group : group_candidates) {
        for (int row : group.*member) {
            ++match_counts[row];
        }
    }
    for (const auto& [row, count] : match_counts) {
        if (count >= required_group_matches) {
            target.insert(row);
        }
    }
}

CandidateRows select_generated_group_candidates(const std::vector<CandidateRows>& groups, int required) {
    CandidateRows candidates;
    if (groups.empty() || std::any_of(groups.begin(), groups.end(), [](const auto& group) {
            return !group.from_terms_index || !group.term_index_coverage_complete;
        })) return candidates;
    const int threshold = std::clamp(required, 1, static_cast<int>(groups.size()));
    for (const auto member : {&CandidateRows::sections, &CandidateRows::facts, &CandidateRows::decisions,
                             &CandidateRows::contracts, &CandidateRows::runbooks}) {
        select_candidate_ids_by_group(candidates.*member, groups, member, threshold);
    }
    candidates.term_index_coverage_complete = true;
    apply_term_candidate_cutoff(candidates, kMaxIndexedCandidateRows);
    return candidates;
}

static CandidateRows select_candidates_by_required_groups(const PreparedTermGroups& term_groups,
                                                    const TermPostingLookup& postings,
                                                    int required_group_matches) {
    CandidateRows candidates;
    if (term_groups.empty()) {
        return candidates;
    }
    std::vector<TermPostingRefs> group_postings;
    group_postings.reserve(term_groups.size());
    for (const PreparedTermGroup& group : term_groups) {
        group_postings.push_back(posting_refs_for_term_group(group, postings));
    }
    const bool has_phrases = std::any_of(term_groups.begin(), term_groups.end(), [](const auto& group) {
        return std::any_of(group.begin(), group.end(), [](const auto& term) { return !term.phrase_keys.empty(); });
    });
    if (!has_phrases && required_group_matches >= static_cast<int>(term_groups.size())) {
        CandidateRows strict_candidates = select_candidates_matching_every_group(group_postings);
        strict_candidates.term_index_coverage_complete = true;
        return strict_candidates;
    }

    std::vector<CandidateRows> group_candidates;
    group_candidates.reserve(term_groups.size());
    for (const PreparedTermGroup& group : term_groups) {
        group_candidates.push_back(candidates_for_term_group(group, postings));
    }
    const int threshold = std::max(1, std::min(required_group_matches,
                                               static_cast<int>(group_candidates.size())));
    select_candidate_ids_by_group(candidates.sections, group_candidates, &CandidateRows::sections, threshold);
    select_candidate_ids_by_group(candidates.facts, group_candidates, &CandidateRows::facts, threshold);
    select_candidate_ids_by_group(candidates.decisions, group_candidates, &CandidateRows::decisions, threshold);
    select_candidate_ids_by_group(candidates.contracts, group_candidates, &CandidateRows::contracts, threshold);
    select_candidate_ids_by_group(candidates.runbooks, group_candidates, &CandidateRows::runbooks, threshold);
    candidates.term_index_coverage_complete = true;
    return candidates;
}

static CandidateRows load_candidate_rows_from_terms_index(const RuntimeOptions& options,
                                                   const fs::path& generated_root,
                                                   const PreparedTermGroups& term_groups,
                                                   int required_group_matches) {
    CandidateRows candidates;
    (void)compact_lexical_index(options);
    if (!generated_file_exists(options, generated_root / "terms.lex")) {
        throw_index_error("index-corrupt", "Generated term index is incomplete.");
    }
    const std::set<std::string> wanted_terms = term_lookup_set(term_groups);
    if (wanted_terms.empty()) {
        return candidates;
    }
    const LoadedTermPostings loaded =
        load_compact_term_postings(options, wanted_terms, prefix_lookup_set(term_groups));
    candidates =
        select_candidates_by_required_groups(term_groups, loaded.lookup, required_group_matches);
    candidates.term_index_coverage_complete =
        candidates.term_index_coverage_complete && loaded.coverage_complete;
    apply_term_candidate_cutoff(candidates, kMaxIndexedCandidateRows);
    return candidates;
}

CandidateRows load_generated_query_candidates(const RuntimeOptions& options,
                                               const PreparedTermGroups& term_groups,
                                               int required_group_matches) {
    const fs::path generated_root = options.out_root / kGeneratedDir;
    const fs::path manifest_path = generated_root / "manifest.json";
    if (!generated_file_exists(options, manifest_path)) {
        throw_index_error("index-unavailable", "Generated KB manifest is missing: " + manifest_path.string());
    }
    return load_candidate_rows_from_terms_index(options, generated_root, term_groups, required_group_matches);
}

std::vector<CandidateRows> load_generated_query_group_candidates(
    const RuntimeOptions& options,
    const PreparedTermGroups& term_groups) {
    const fs::path generated_root = options.out_root / kGeneratedDir;
    const fs::path manifest_path = generated_root / "manifest.json";
    if (!generated_file_exists(options, manifest_path)) {
        throw_index_error("index-unavailable", "Generated KB manifest is missing: " + manifest_path.string());
    }
    (void)compact_lexical_index(options);
    if (!generated_file_exists(options, generated_root / "terms.lex")) {
        throw_index_error("index-corrupt", "Generated term index is incomplete.");
    }

    const std::set<std::string> wanted_terms = term_lookup_set(term_groups);
    const LoadedTermPostings loaded =
        load_compact_term_postings(options, wanted_terms, prefix_lookup_set(term_groups));
    std::vector<CandidateRows> groups;
    groups.reserve(term_groups.size());
    for (const PreparedTermGroup& group : term_groups) {
        CandidateRows candidates = candidates_for_term_group(group, loaded.lookup);
        candidates.term_index_coverage_complete =
            loaded.coverage_complete;
        apply_term_candidate_cutoff(candidates, kMaxIndexedCandidateRows);
        groups.push_back(std::move(candidates));
    }
    return groups;
}

std::optional<std::vector<CandidateRows>> recover_document_coverage_candidates(
    const RuntimeOptions& options, const PreparedTermGroups& groups, int required,
    const std::set<std::string>& documents, const CandidateRows* ordinary) {
    const auto wanted = term_lookup_set(groups);
    (void)compact_lexical_index(options);
    auto loaded = load_compact_term_postings(options, wanted, prefix_lookup_set(groups));
    if (!loaded.coverage_complete) return std::nullopt;
    // Independent source records cannot contribute whole-document coverage.
    // Drop their postings before materializing any candidate sets. Retain only
    // one raw document group at a time; qualify owners before the final cap.
    for (auto& [term, posting] : loaded.lookup) { (void)term; posting.facts.clear(); }
    return filter_document_coverage_postings(options, groups.size(), [&](std::size_t group) {
        auto rows = candidates_for_term_group(groups[group], loaded.lookup);
        rows.from_terms_index = true;
        rows.term_index_coverage_complete = true;
        return rows;
    }, required, documents, ordinary);
}

static std::map<int, RowOffset> load_wanted_row_offsets(const RuntimeOptions& options, const fs::path& offsets_path,
                                                 const std::set<int>& wanted_rows) {
    std::map<int, RowOffset> offsets;
    auto wanted = wanted_rows.lower_bound(0);
    if (wanted == wanted_rows.end()) {
        return offsets;
    }
    GeneratedInput input(options, offsets_path);
    if (!input) {
        throw std::runtime_error("Cannot read " + offsets_path.string());
    }
    BoundedGeneratedLineReader reader(input, kMaxGeneratedOffsetIndexLineBytes,
                                      "Generated row offset index row");
    std::string line;
    while (wanted != wanted_rows.end() && reader.read(line)) {
        RowOffset offset;
        if (!parse_row_offset_line(line, offset)) {
            continue;
        }
        while (wanted != wanted_rows.end() && *wanted < offset.row) {
            ++wanted;
        }
        if (wanted != wanted_rows.end() && *wanted == offset.row) {
            offsets.emplace(offset.row, offset);
            ++wanted;
        }
    }
    return offsets;
}

static void for_each_candidate_jsonl_line(const RuntimeOptions& options, const fs::path& data_path,
                                   const fs::path& offsets_path,
                                   const std::set<int>& row_ids,
                                   const std::function<void(const std::string&)>& handle) {
    if (row_ids.empty()) {
        return;
    }
    if (!generated_file_exists(options, data_path) || !generated_file_exists(options, offsets_path)) {
        throw_index_error("index-corrupt", "Generated row index is incomplete.");
    }
    const std::map<int, RowOffset> offsets = load_wanted_row_offsets(options, offsets_path, row_ids);
    const std::size_t wanted_rows = static_cast<std::size_t>(
        std::count_if(row_ids.begin(), row_ids.end(), [](int row) { return row >= 0; }));
    if (offsets.size() != wanted_rows) {
        throw_index_error("index-corrupt", "Generated row index does not cover every candidate row.");
    }
    GeneratedInput input(options, data_path);
    if (!input) {
        throw std::runtime_error("Cannot read " + data_path.string());
    }
    const long long data_bytes = data_file_size(input, data_path);
    for (const auto& [row, offset] : offsets) {
        (void)row;
        require_valid_data_range(offset.offset, offset.bytes, data_bytes,
                                 "Generated row offset index");
        input.clear();
        input.seekg(offset.offset);
        std::string line(static_cast<std::size_t>(offset.bytes), '\0');
        input.read(&line[0], static_cast<std::streamsize>(line.size()));
        if (input.gcount() != static_cast<std::streamsize>(line.size())) {
            throw_index_error("index-corrupt", "Generated row index points outside its data file.");
        }
        line.resize(static_cast<std::size_t>(input.gcount()));
        line = trim(line);
        if (line.empty()) {
            throw_index_error("index-corrupt", "Generated row index points to an empty row.");
        }
        handle(line);
    }
}

static std::string indexed_jsonl_line_at(std::istream& input,
                                         long long data_bytes,
                                         const RowOffset& offset) {
    require_valid_data_range(offset.offset, offset.bytes, data_bytes,
                             "Generated row offset index");
    input.clear();
    input.seekg(offset.offset);
    std::string line(static_cast<std::size_t>(offset.bytes), '\0');
    input.read(line.data(), static_cast<std::streamsize>(line.size()));
    if (input.gcount() != static_cast<std::streamsize>(line.size())) {
        throw_index_error("index-corrupt", "Generated row index points outside its data file.");
    }
    return trim(line);
}

static bool find_first_path_index_line(std::istream& index_input,
                                       long long index_bytes,
                                       std::istream& data_input,
                                       long long data_bytes,
                                       const std::string& wanted_path,
                                       const std::function<std::string(const std::string&)>& path_from_line,
                                       long long& first_index_offset) {
    long long lower = 0;
    long long upper = index_bytes;
    while (lower < upper) {
        const long long probe = lower + (upper - lower) / 2;
        OffsetIndexLine index_line;
        if (!read_offset_index_line(index_input, index_bytes, probe, index_line)) {
            upper = probe;
            continue;
        }
        RowOffset row_offset;
        if (!parse_row_offset_line(index_line.text, row_offset)) {
            throw_index_error("index-corrupt", "Generated row offset index is invalid.");
        }
        const std::string row = indexed_jsonl_line_at(data_input, data_bytes, row_offset);
        const std::string path = path_from_line(row);
        if (path < wanted_path) {
            if (index_line.next_offset <= lower) {
                throw_index_error("index-corrupt", "Generated path index search did not advance.");
            }
            lower = index_line.next_offset;
        } else {
            if (index_line.offset >= upper) {
                throw_index_error("index-corrupt", "Generated path index search did not advance.");
            }
            upper = index_line.offset;
        }
    }
    if (lower >= index_bytes) {
        return false;
    }
    OffsetIndexLine first_line;
    if (!read_offset_index_line(index_input, index_bytes, lower, first_line)) {
        return false;
    }
    RowOffset first_row;
    if (!parse_row_offset_line(first_line.text, first_row)) {
        throw_index_error("index-corrupt", "Generated row offset index is invalid.");
    }
    const std::string row = indexed_jsonl_line_at(data_input, data_bytes, first_row);
    if (path_from_line(row) != wanted_path) {
        return false;
    }
    first_index_offset = first_line.offset;
    return true;
}

static void probe_generated_path_store(
    const RuntimeOptions& options,
    const fs::path& data_path,
    const fs::path& index_path,
    const std::string& wanted_path,
    std::size_t max_rows,
    const std::function<std::string(const std::string&)>& path_from_line,
    const std::function<void(const std::string&, int)>& retain,
    GeneratedPathProbe& probe) {
    if (!generated_file_exists(options, data_path) || !generated_file_exists(options, index_path)) {
        throw_index_error("index-corrupt", "Generated path probe index is incomplete.");
    }
    GeneratedInput index_input(options, index_path);
    GeneratedInput data_input(options, data_path);
    if (!index_input || !data_input) {
        throw std::runtime_error("Cannot open generated path probe store.");
    }
    const long long index_bytes = data_file_size(index_input, index_path);
    const long long data_bytes = data_file_size(data_input, data_path);
    if (index_bytes == 0) {
        return;
    }
    long long first_index_offset = 0;
    if (!find_first_path_index_line(index_input, index_bytes, data_input, data_bytes,
                                    wanted_path, path_from_line, first_index_offset)) {
        return;
    }

    index_input.clear();
    index_input.seekg(first_index_offset);
    BoundedGeneratedLineReader reader(index_input, kMaxGeneratedOffsetIndexLineBytes,
                                      "Generated row offset index row");
    std::string index_line;
    int previous_row = -1;
    std::size_t retained = 0;
    while (reader.read(index_line)) {
        RowOffset row_offset;
        if (!parse_row_offset_line(index_line, row_offset) ||
            (previous_row >= 0 && row_offset.row != previous_row + 1)) {
            throw_index_error("index-corrupt", "Generated row offset index is not contiguous.");
        }
        previous_row = row_offset.row;
        const std::string row = indexed_jsonl_line_at(data_input, data_bytes, row_offset);
        if (path_from_line(row) != wanted_path) {
            break;
        }
        if (retained >= max_rows) {
            probe.complete = false;
            break;
        }
        retain(row, row_offset.row);
        ++retained;
        ++probe.rows_examined;
    }
}

GeneratedPathProbe probe_generated_path(const RuntimeOptions& options,
                                        const std::string& path,
                                        std::size_t max_rows_per_store) {
    GeneratedPathProbe probe;
    const fs::path generated = options.out_root / kGeneratedDir;
    probe_generated_path_store(
        options,
        generated / "sections.jsonl", generated / "sections.index.tsv", path,
        max_rows_per_store,
        [](const std::string& line) { return parse_section_row(line).path; },
        [&](const std::string& line, int id) {
            probe.sections.push_back(parse_section_row(line));
            probe.row_ids.sections.insert(id);
        }, probe);
    auto probe_fact_store = [&](const std::string& data_name, const std::string& index_name,
                                const auto& parser, std::set<int>& ids) {
        probe_generated_path_store(
            options,
            generated / data_name, generated / index_name, path, max_rows_per_store,
            [&](const std::string& line) { return parser(line).path; },
            [&](const std::string& line, int id) {
                probe.facts.push_back(parser(line));
                ids.insert(id);
            }, probe);
    };
    probe_fact_store("facts.jsonl", "facts.index.tsv", parse_generated_fact_row, probe.row_ids.facts);
    probe_fact_store("decisions.jsonl", "decisions.index.tsv", parse_decision_as_fact_row, probe.row_ids.decisions);
    probe_fact_store("contracts.jsonl", "contracts.index.tsv", parse_typed_reference_as_fact_row, probe.row_ids.contracts);
    probe_fact_store("runbooks.jsonl", "runbooks.index.tsv", parse_typed_reference_as_fact_row, probe.row_ids.runbooks);
    return probe;
}

void for_each_generated_section_matching_terms(const RuntimeOptions& options,
                                               const CandidateRows& candidates,
                                               const PreparedTermGroups&,
                                               int,
                                               const std::function<void(const SectionRow&)>& handle) {
    const fs::path generated_root = options.out_root / kGeneratedDir;
    if (candidates.from_terms_index) {
        for_each_candidate_jsonl_line(options, generated_root / "sections.jsonl", generated_root / "sections.index.tsv",
                                      candidates.sections,
                                      [&](const std::string& line) {
            SectionRow section = parse_section_row(line);
            if (!section.path.empty()) {
                handle(section);
            }
        });
        return;
    }
    for_each_snapshot_line(options, generated_root / "sections.jsonl", [&](const std::string& line) {
        SectionRow section = parse_section_row(line);
        if (!section.path.empty()) {
            handle(section);
        }
    });
}

void for_each_generated_fact_matching_terms(const RuntimeOptions& options,
                                            const CandidateRows& candidates,
                                            const PreparedTermGroups&,
                                            int,
                                            const std::function<void(const FactRow&)>& handle) {
    const fs::path generated_root = options.out_root / kGeneratedDir;
    if (candidates.from_terms_index) {
        for_each_candidate_jsonl_line(options, generated_root / "facts.jsonl", generated_root / "facts.index.tsv",
                                      candidates.facts,
                                      [&](const std::string& line) {
            FactRow fact = parse_generated_fact_row(line);
            if (!fact.id.empty()) {
                handle(fact);
            }
        });
        for_each_candidate_jsonl_line(options, generated_root / "decisions.jsonl", generated_root / "decisions.index.tsv",
                                      candidates.decisions,
                                      [&](const std::string& line) {
            FactRow fact = parse_decision_as_fact_row(line);
            if (!fact.id.empty()) {
                handle(fact);
            }
        });
        for_each_candidate_jsonl_line(options, generated_root / "contracts.jsonl", generated_root / "contracts.index.tsv",
                                      candidates.contracts,
                                      [&](const std::string& line) {
            FactRow fact = parse_typed_reference_as_fact_row(line);
            if (!fact.id.empty()) {
                handle(fact);
            }
        });
        for_each_candidate_jsonl_line(options, generated_root / "runbooks.jsonl", generated_root / "runbooks.index.tsv",
                                      candidates.runbooks,
                                      [&](const std::string& line) {
            FactRow fact = parse_typed_reference_as_fact_row(line);
            if (!fact.id.empty()) {
                handle(fact);
            }
        });
        return;
    }
    for_each_snapshot_line(options, generated_root / "facts.jsonl", [&](const std::string& line) {
        FactRow fact = parse_generated_fact_row(line);
        if (!fact.id.empty()) {
            handle(fact);
        }
    });
    for_each_snapshot_line(options, generated_root / "decisions.jsonl", [&](const std::string& line) {
        FactRow fact = parse_decision_as_fact_row(line);
        if (!fact.id.empty()) {
            handle(fact);
        }
    });
    for (const char* file : {"contracts.jsonl", "runbooks.jsonl"}) {
        for_each_snapshot_line(options, generated_root / file, [&](const std::string& line) {
            FactRow fact = parse_typed_reference_as_fact_row(line);
            if (!fact.id.empty()) {
                handle(fact);
            }
        });
    }
}

} // namespace shin::context
