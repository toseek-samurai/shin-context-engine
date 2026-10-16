#pragma once

#include "model.h"
#include <filesystem>

namespace shin::server {

// Separate from the DB lease: a delayed writer retains this lock until it exits.
class CandidateArtifactLock {
public:
    enum class Mode { create_writer, inspect_existing, cleanup_existing };
    CandidateArtifactLock(const std::filesystem::path& state_root, const std::string& state_identity,
                          std::uint64_t job_id, const std::string& artifact_key, Mode mode);
    ~CandidateArtifactLock();
    CandidateArtifactLock(const CandidateArtifactLock&) = delete;
    CandidateArtifactLock& operator=(const CandidateArtifactLock&) = delete;
    void verify() const;
private:
    std::filesystem::path root_;
    std::filesystem::path path_;
    std::string root_identity_;
    int descriptor_ = -1;
};

std::string verify_candidate_layout(const std::filesystem::path& root, std::uintmax_t maximum_file_bytes);
std::string candidate_artifact_name(std::uint64_t job_id, const std::string& artifact_key);
std::string candidate_namespace(const std::filesystem::path& root, const std::string& identity);
std::string candidate_execution_fingerprint(const std::string& route_fingerprint,
    const std::filesystem::path& root, const std::string& identity,
    std::uintmax_t maximum_payload, std::uintmax_t maximum_file);

} // namespace shin::server
