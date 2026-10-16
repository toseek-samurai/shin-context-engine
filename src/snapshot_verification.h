#pragma once

#include "context_types.h"

namespace shin::context {

struct SnapshotFile {
    std::string path;
    std::uintmax_t bytes = 0;
    std::string content_hash;
};

struct VerifiedSnapshot {
    fs::path out_root;
    std::string manifest_content;
    Json manifest = Json::object();
    std::string snapshot_id;
    std::string schema_version;
    std::string profile;
    std::string profile_config_hash;
    std::string generator_version;
    Json row_counts = Json::object();
    std::map<std::string, std::string> sources;
    std::map<std::string, std::string> source_facts;
    std::map<std::string, SnapshotFile> files;
    // Byte observations from this load, reusable only with matching metadata
    // and the same output root; callers still fence content before completion.
    std::map<std::string, FileFingerprint> fingerprints;
};

fs::path canonical_snapshot_root(const std::string& value,
                                 const std::string& option);
VerifiedSnapshot load_verified_snapshot(
    const fs::path& out_root,
    std::uintmax_t maximum_payload_bytes = std::numeric_limits<std::uintmax_t>::max(),
    std::uintmax_t maximum_file_bytes = std::numeric_limits<std::uintmax_t>::max());
void verify_snapshot_manifest_file(const fs::path& out_root);
void verify_snapshot_filesystem(const fs::path& out_root, const std::set<std::string>& expected_files);
Json verified_snapshot_description(const VerifiedSnapshot& snapshot);

}  // namespace shin::context
