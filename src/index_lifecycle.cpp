#include "context_internal.h"
#include "generated_io.h"
#include "snapshot_verification.h"

namespace shin::context {
namespace {

constexpr std::size_t kMaxCacheEntries = 4096;
constexpr std::size_t kMaxKeepKeys = 256;

struct CacheOptions {
    fs::path cache_dir;
    std::set<std::string> keep_keys;
    bool prune_unreferenced = false;
    bool apply = false;
    bool strict = false;
};

struct CacheEntry {
    std::string key;
    fs::path path;
    std::string state;
    bool managed = false;
    bool safe = true;
    std::uintmax_t snapshot_bytes = 0;
    std::optional<VerifiedSnapshot> snapshot;
    Json error = nullptr;
};

Json lifecycle_error_json(const CliError& error) {
    return {
        {"code", error.code()},
        {"category", error.category()},
        {"message", error.what()},
        {"retryable", error.retryable()},
        {"remediation", error.remediation()}
    };
}

Json lifecycle_error_json(const std::string& code,
                          const std::string& category,
                          const std::string& message,
                          const std::string& remediation) {
    return {
        {"code", code},
        {"category", category},
        {"message", message},
        {"retryable", false},
        {"remediation", remediation}
    };
}

fs::path canonical_cache_root(const fs::path& input) {
    if (input.empty()) {
        throw_usage_error("missing-option", "index lifecycle commands require --cache-dir PATH.");
    }
    std::error_code error;
    const fs::path absolute = fs::absolute(input).lexically_normal();
    const fs::file_status status = fs::symlink_status(absolute, error);
    if (error || fs::is_symlink(status) || !fs::is_directory(status)) {
        throw_usage_error("invalid-cache-root",
                          "--cache-dir must name an existing non-symlink directory.");
    }
    const fs::path canonical = fs::canonical(absolute, error);
    if (error || canonical == canonical.root_path()) {
        throw_usage_error(
            "unsafe-cache-root",
            "--cache-dir must resolve to a non-root directory.",
            "Pass the canonical dedicated Shin cache directory explicitly.");
    }
    return canonical;
}

CacheOptions parse_cache_options(const std::vector<std::string>& args,
                                 bool prune_command) {
    CacheOptions options;
    bool cache_seen = false;
    bool unreferenced_seen = false;
    bool apply_seen = false;
    bool strict_seen = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& argument = args[index];
        auto read_value = [&](const std::string& option) -> const std::string& {
            if (index + 1 >= args.size()) {
                throw_usage_error("missing-option-value", option + " requires a value.");
            }
            return args[++index];
        };
        if (argument == "--cache-dir") {
            if (cache_seen) {
                throw_usage_error("duplicate-option",
                                  "The command accepts --cache-dir only once.");
            }
            options.cache_dir = read_value(argument);
            cache_seen = true;
        } else if (argument == "--keep-key") {
            if (!prune_command) {
                throw_usage_error("unsupported-option",
                                  "index-list does not accept --keep-key.");
            }
            const std::string& key = read_value(argument);
            if (!portable_identifier_valid(key)) {
                throw_usage_error("invalid-index-key",
                                  "--keep-key must be a portable index identifier.");
            }
            if (!options.keep_keys.insert(key).second) {
                throw_usage_error("duplicate-index-key",
                                  "The same --keep-key value was provided more than once.");
            }
            if (options.keep_keys.size() > kMaxKeepKeys) {
                throw CliError("keep-key-limit-exceeded", "resource",
                               "index-prune accepts at most 256 protected keys.", 1, false,
                               "Use one bounded cache root per project group.");
            }
        } else if (argument == "--prune-unreferenced") {
            if (!prune_command || unreferenced_seen) {
                throw_usage_error(!prune_command ? "unsupported-option" : "duplicate-option",
                                  !prune_command
                                      ? "index-list does not accept --prune-unreferenced."
                                      : "index-prune accepts --prune-unreferenced only once.");
            }
            options.prune_unreferenced = true;
            unreferenced_seen = true;
        } else if (argument == "--apply") {
            if (!prune_command || apply_seen) {
                throw_usage_error(!prune_command ? "unsupported-option" : "duplicate-option",
                                  !prune_command
                                      ? "index-list does not accept --apply."
                                      : "index-prune accepts --apply only once.");
            }
            options.apply = true;
            apply_seen = true;
        } else if (argument == "--strict") {
            if (strict_seen) {
                throw_usage_error("duplicate-option",
                                  "The command accepts --strict only once.");
            }
            options.strict = true;
            strict_seen = true;
        } else {
            throw_usage_error("unsupported-option",
                              "Unsupported index lifecycle option: " + argument + ".");
        }
    }
    if (!cache_seen) {
        throw_usage_error("missing-option", "The command requires --cache-dir PATH.");
    }
    if (options.prune_unreferenced && options.keep_keys.empty()) {
        throw_usage_error(
            "missing-keep-key",
            "--prune-unreferenced requires at least one --keep-key safeguard.",
            "Protect every active marker-derived index key before pruning unreferenced indexes.");
    }
    options.cache_dir = canonical_cache_root(options.cache_dir);
    return options;
}

std::uintmax_t verified_snapshot_bytes(const VerifiedSnapshot& snapshot) {
    std::uintmax_t bytes = snapshot.manifest_content.size();
    for (const auto& [path, file] : snapshot.files) {
        (void)path;
        if (file.bytes > std::numeric_limits<std::uintmax_t>::max() - bytes) {
            return std::numeric_limits<std::uintmax_t>::max();
        }
        bytes += file.bytes;
    }
    return bytes;
}

std::vector<CacheEntry> inspect_cache(const fs::path& cache_dir) {
    std::vector<CacheEntry> entries;
    std::error_code error;
    fs::directory_iterator iterator(cache_dir, fs::directory_options::none, error);
    const fs::directory_iterator end;
    if (error) {
        throw CliError("cache-enumeration-failed", "filesystem",
                       "Cannot enumerate the Shin cache root: " + cache_dir.string() + ".",
                       1, true, "Verify cache permissions and retry.");
    }
    while (iterator != end) {
        if (entries.size() >= kMaxCacheEntries) {
            throw CliError("cache-entry-limit-exceeded", "resource",
                           "The Shin cache root exceeds 4096 immediate entries.", 1, false,
                           "Partition the cache into smaller explicitly managed roots.");
        }
        CacheEntry entry;
        entry.path = iterator->path();
        entry.key = entry.path.filename().string();
        const fs::file_status status = iterator->symlink_status(error);
        if (error) {
            throw CliError("cache-enumeration-failed", "filesystem",
                           "Cannot inspect a Shin cache entry.", 1, true,
                           "Verify cache permissions and retry.");
        }
        if (!portable_identifier_valid(entry.key)) {
            entry.state = "unmanaged";
            entry.safe = !fs::is_symlink(status);
        } else if (fs::is_symlink(status) || !fs::is_directory(status)) {
            entry.state = "unsafe-entry";
            entry.safe = false;
            entry.error = lifecycle_error_json(
                "unsafe-cache-entry", "filesystem",
                "Cache entry must be a non-symlink directory: " + entry.path.string() + ".",
                "Remove or relocate the unmanaged entry manually; index-prune will not touch it.");
        } else {
            const fs::path manifest = entry.path / kGeneratedDir / "manifest.json";
            const fs::file_status manifest_status = fs::symlink_status(manifest, error);
            if (!fs::exists(manifest_status) &&
                (!error || error == std::errc::no_such_file_or_directory)) {
                entry.state = "empty-shell";
                error.clear();
            } else if (error) {
                entry.state = "corrupt";
                entry.managed = true;
                entry.safe = false;
                entry.error = lifecycle_error_json(
                    "cache-manifest-inspection-failed", "filesystem",
                    "Cannot inspect generated manifest: " + manifest.string() + ".",
                    "Verify permissions; index-prune will not touch the entry.");
                error.clear();
            } else if (fs::is_symlink(manifest_status) ||
                       !fs::is_regular_file(manifest_status)) {
                entry.state = "corrupt";
                entry.managed = true;
                entry.safe = false;
                entry.error = lifecycle_error_json(
                    "unsafe-cache-manifest", "index",
                    "Generated manifest must be a non-symlink regular file: " +
                        manifest.string() + ".",
                    "Inspect the entry manually; index-prune never removes corrupt snapshots.");
            } else {
                entry.managed = true;
                try {
                    shin::generated_io::IndexLock lock(
                        entry.path, shin::generated_io::LockMode::shared_existing);
                    entry.snapshot = load_verified_snapshot(entry.path);
                    entry.snapshot_bytes = verified_snapshot_bytes(*entry.snapshot);
                    entry.state = (entry.snapshot->generator_version == kGeneratorVersion ||
                        entry.snapshot->generator_version == kLegacyGeneratorVersion)
                                      ? "current"
                                      : "incompatible-generator";
                } catch (const CliError& caught) {
                    entry.state = "corrupt";
                    entry.safe = false;
                    entry.error = lifecycle_error_json(caught);
                } catch (const std::exception& caught) {
                    entry.state = "corrupt";
                    entry.safe = false;
                    entry.error = lifecycle_error_json(
                        "cache-inspection-failed", "runtime", caught.what(),
                        "Inspect the entry manually; index-prune never removes unverified snapshots.");
                }
            }
        }
        entries.push_back(std::move(entry));
        iterator.increment(error);
        if (error) {
            throw CliError("cache-enumeration-failed", "filesystem",
                           "Cannot enumerate the complete Shin cache root.", 1, true,
                           "Verify cache permissions and retry.");
        }
    }
    std::sort(entries.begin(), entries.end(), [](const CacheEntry& left,
                                                 const CacheEntry& right) {
        return left.key < right.key;
    });
    return entries;
}

Json cache_entry_json(const CacheEntry& entry) {
    Json output = {
        {"key", entry.key},
        {"path", entry.path.generic_string()},
        {"state", entry.state},
        {"managed", entry.managed},
        {"safe", entry.safe},
        {"snapshot_bytes", entry.snapshot_bytes},
        {"error", entry.error}
    };
    if (entry.snapshot.has_value()) {
        output["snapshot"] = verified_snapshot_description(*entry.snapshot);
        output["compatible"] = (entry.snapshot->generator_version == kGeneratorVersion ||
                        entry.snapshot->generator_version == kLegacyGeneratorVersion);
    } else {
        output["snapshot"] = nullptr;
        output["compatible"] = nullptr;
    }
    return output;
}

Json inventory_summary(const std::vector<CacheEntry>& entries) {
    std::size_t managed = 0;
    std::size_t current = 0;
    std::size_t incompatible = 0;
    std::size_t corrupt = 0;
    std::size_t empty_shell = 0;
    std::size_t unsafe = 0;
    std::size_t unmanaged = 0;
    std::uintmax_t snapshot_bytes = 0;
    for (const CacheEntry& entry : entries) {
        if (entry.managed) {
            ++managed;
        }
        if (entry.state == "current") {
            ++current;
        } else if (entry.state == "incompatible-generator") {
            ++incompatible;
        } else if (entry.state == "corrupt") {
            ++corrupt;
        } else if (entry.state == "empty-shell") {
            ++empty_shell;
        } else if (entry.state == "unsafe-entry") {
            ++unsafe;
        } else if (entry.state == "unmanaged") {
            ++unmanaged;
        }
        if (entry.snapshot_bytes >
            std::numeric_limits<std::uintmax_t>::max() - snapshot_bytes) {
            snapshot_bytes = std::numeric_limits<std::uintmax_t>::max();
        } else {
            snapshot_bytes += entry.snapshot_bytes;
        }
    }
    return {
        {"entries", entries.size()},
        {"managed", managed},
        {"current", current},
        {"incompatible_generator", incompatible},
        {"corrupt", corrupt},
        {"empty_shell", empty_shell},
        {"unsafe", unsafe},
        {"unmanaged", unmanaged},
        {"snapshot_bytes", snapshot_bytes}
    };
}

bool inventory_clean(const Json& summary) {
    return summary["incompatible_generator"].get<int>() == 0 &&
           summary["corrupt"].get<int>() == 0 &&
           summary["unsafe"].get<int>() == 0;
}

}  // namespace

int run_index_list(const std::vector<std::string>& args) {
    const CacheOptions options = parse_cache_options(args, false);
    const std::vector<CacheEntry> entries = inspect_cache(options.cache_dir);
    const Json summary = inventory_summary(entries);
    const bool clean = inventory_clean(summary);
    const bool ok = !options.strict || clean;

    Json output = make_result_document("shin.index-list.v1", "index-list", ok);
    output["cache_root"] = options.cache_dir.generic_string();
    output["current_generator"] = kGeneratorVersion;
    output["strict"] = options.strict;
    output["clean"] = clean;
    output["policy"] = {
        {"scope", "immediate-child-managed-indexes-v1"},
        {"snapshot_mutation", false},
        {"consumer_access", false},
        {"network_access", false}
    };
    output["summary"] = summary;
    output["entries"] = Json::array();
    for (const CacheEntry& entry : entries) {
        output["entries"].push_back(cache_entry_json(entry));
    }
    emit_json_document(std::cout, output);
    return ok ? 0 : 1;
}

int run_index_prune(const std::vector<std::string>& args) {
    const CacheOptions options = parse_cache_options(args, true);
    std::vector<CacheEntry> entries = inspect_cache(options.cache_dir);
    std::set<std::string> observed_keys;
    for (const CacheEntry& entry : entries) {
        observed_keys.insert(entry.key);
    }
    std::vector<std::string> missing_keep_keys;
    for (const std::string& key : options.keep_keys) {
        if (!observed_keys.contains(key)) {
            missing_keep_keys.push_back(key);
        }
    }

    Json actions = Json::array();
    std::size_t candidates = 0;
    std::size_t pruned = 0;
    std::size_t failures = 0;
    std::uintmax_t bytes_reclaimable = 0;
    std::uintmax_t bytes_reclaimed = 0;
    for (CacheEntry& entry : entries) {
        if (!missing_keep_keys.empty()) {
            break;
        }
        const bool protected_key = options.keep_keys.contains(entry.key);
        const bool incompatible = entry.state == "incompatible-generator";
        const bool unreferenced = options.prune_unreferenced &&
                                  entry.state == "current" && !protected_key;
        const bool candidate = entry.snapshot.has_value() && !protected_key &&
                               (incompatible || unreferenced);
        if (!candidate) {
            continue;
        }
        ++candidates;
        if (entry.snapshot_bytes >
            std::numeric_limits<std::uintmax_t>::max() - bytes_reclaimable) {
            bytes_reclaimable = std::numeric_limits<std::uintmax_t>::max();
        } else {
            bytes_reclaimable += entry.snapshot_bytes;
        }
        Json action = {
            {"key", entry.key},
            {"path", entry.path.generic_string()},
            {"reason", incompatible ? "incompatible-generator" : "unreferenced"},
            {"snapshot_id", entry.snapshot->snapshot_id},
            {"generator_version", entry.snapshot->generator_version},
            {"snapshot_bytes", entry.snapshot_bytes},
            {"apply_requested", options.apply},
            {"applied", false},
            {"error", nullptr}
        };
        if (options.apply) {
            try {
                shin::generated_io::prune_snapshot(
                    entry.path, kGeneratedDir, entry.snapshot->manifest_content);
                action["applied"] = true;
                ++pruned;
                if (entry.snapshot_bytes >
                    std::numeric_limits<std::uintmax_t>::max() - bytes_reclaimed) {
                    bytes_reclaimed = std::numeric_limits<std::uintmax_t>::max();
                } else {
                    bytes_reclaimed += entry.snapshot_bytes;
                }
            } catch (const std::exception& caught) {
                ++failures;
                action["error"] = lifecycle_error_json(
                    "index-prune-failed", "filesystem", caught.what(),
                    "Re-run index-list, verify no publisher or reader is active, and retry.");
            }
        }
        actions.push_back(std::move(action));
    }

    const Json before = inventory_summary(entries);
    const std::size_t protected_incompatible = static_cast<std::size_t>(std::count_if(
        entries.begin(), entries.end(), [&](const CacheEntry& entry) {
            return entry.state == "incompatible-generator" &&
                   options.keep_keys.contains(entry.key);
        }));
    const std::size_t remaining_candidates = candidates - pruned;
    const bool integrity_clean = before["corrupt"].get<int>() == 0 &&
                                 before["unsafe"].get<int>() == 0;
    const bool clean = integrity_clean && missing_keep_keys.empty() &&
                       remaining_candidates == 0 && protected_incompatible == 0;
    const bool ok = failures == 0 && missing_keep_keys.empty() &&
                    (!options.strict || clean);

    Json output = make_result_document("shin.index-prune.v1", "index-prune", ok);
    output["cache_root"] = options.cache_dir.generic_string();
    output["current_generator"] = kGeneratorVersion;
    output["apply"] = options.apply;
    output["strict"] = options.strict;
    output["prune_unreferenced"] = options.prune_unreferenced;
    output["keep_keys"] = options.keep_keys;
    output["missing_keep_keys"] = missing_keep_keys;
    output["clean"] = clean;
    output["policy"] = {
        {"scope", "verified-immediate-child-snapshots-v1"},
        {"dry_run_default", true},
        {"removes_generated_snapshot_only", true},
        {"removes_corrupt_or_unmanaged", false},
        {"consumer_access", false},
        {"network_access", false}
    };
    output["summary"] = {
        {"entries", entries.size()},
        {"managed", before["managed"]},
        {"current", before["current"]},
        {"incompatible_generator", before["incompatible_generator"]},
        {"corrupt", before["corrupt"]},
        {"unsafe", before["unsafe"]},
        {"unmanaged", before["unmanaged"]},
        {"protected_incompatible", protected_incompatible},
        {"candidates", candidates},
        {"pruned", pruned},
        {"failures", failures},
        {"remaining_candidates", remaining_candidates},
        {"bytes_reclaimable", bytes_reclaimable},
        {"bytes_reclaimed", bytes_reclaimed}
    };
    output["actions"] = std::move(actions);
    emit_json_document(std::cout, output);
    return ok ? 0 : 1;
}

}  // namespace shin::context
