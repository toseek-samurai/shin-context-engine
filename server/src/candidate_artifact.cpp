#include "shin/server/candidate_artifact.h"
#include "shin/server/config.h"
#include "shin/server/route_files.h"
#include "shin/server/version.h"
#include "context_internal.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits>
#include <set>

namespace shin::server {
namespace {
[[noreturn]] void unsafe_lock() {
    throw ServiceError(409, "candidate-lock-unsafe", "Candidate lifetime lock is unavailable or unsafe.");
}

[[noreturn]] void invalid_artifact() {
    throw ServiceError(409, "candidate-artifact-invalid", "Candidate artifact layout is invalid.");
}

bool safe_lock_file(const struct stat& status) {
    return S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
           (status.st_mode & 0077) == 0 && status.st_nlink == 1 && status.st_size == 0;
}
} // namespace

CandidateArtifactLock::CandidateArtifactLock(const std::filesystem::path& state_root,
                                             const std::string& state_identity,
                                             std::uint64_t job_id, const std::string& artifact_key, Mode mode)
    : root_(state_root), root_identity_(state_identity) {
    path_ = root_ / ("." + candidate_artifact_name(job_id, artifact_key) + ".lock");
    if (route_directory_identity(root_) != root_identity_) unsafe_lock();
    const bool writer = mode == Mode::create_writer;
    descriptor_ = ::open(path_.c_str(), O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
            (writer ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY), 0600);
    if (descriptor_ < 0) {
        if (!writer && errno == ENOENT)
            throw ServiceError(409, "candidate-lock-missing", "Candidate lifetime lock is missing.");
        if (writer && errno == EEXIST)
            throw ServiceError(409, "candidate-lock-exists", "Candidate lifetime lock already exists.");
        unsafe_lock();
    }
    try {
        verify();
        int result = 0;
        do { result = ::flock(descriptor_, (mode == Mode::inspect_existing ? LOCK_SH : LOCK_EX) | LOCK_NB); }
        while (result != 0 && errno == EINTR);
        if (result != 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
                throw ServiceError(409, "candidate-artifact-busy", "Candidate files are held by another operation.");
            unsafe_lock();
        }
        verify();
    } catch (...) {
        ::close(descriptor_);
        descriptor_ = -1;
        throw;
    }
}

std::string candidate_artifact_name(std::uint64_t job_id, const std::string& artifact_key) {
    if (job_id == 0 || job_id > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        artifact_key.size() != 64 || !std::all_of(artifact_key.begin(), artifact_key.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) unsafe_lock();
    return "job-" + std::to_string(job_id) + "-" + artifact_key;
}

CandidateArtifactLock::~CandidateArtifactLock() {
    if (descriptor_ >= 0) ::close(descriptor_);
    // Never unlink a lock inode: a delayed process may still own another descriptor.
}

std::string candidate_namespace(const std::filesystem::path& root, const std::string& identity) {
    return digest_hex(sha256(Json{{"schema_version", "shin.artifact-namespace.v1"},
        {"root", root.string()}, {"identity", identity}}.dump()));
}

std::string candidate_execution_fingerprint(const std::string& route_fingerprint,
    const std::filesystem::path& root, const std::string& identity,
    std::uintmax_t maximum_payload, std::uintmax_t maximum_file) {
    return digest_hex(sha256(Json{{"route", route_fingerprint}, {"version", kServerVersion},
        {"state_dir", root.string()}, {"state_identity", identity},
        {"maximum_payload", maximum_payload}, {"maximum_file", maximum_file}}.dump()));
}

void CandidateArtifactLock::verify() const {
    struct stat root{}, held{}, named{};
    if (route_directory_identity(root_) != root_identity_ || ::lstat(root_.c_str(), &root) != 0 ||
        root.st_uid != ::geteuid() || (root.st_mode & 0077) != 0 ||
        ::fstat(descriptor_, &held) != 0 || ::lstat(path_.c_str(), &named) != 0 ||
        !safe_lock_file(held) || !safe_lock_file(named) ||
        held.st_dev != named.st_dev || held.st_ino != named.st_ino) unsafe_lock();
}

std::string verify_candidate_layout(const std::filesystem::path& root, std::uintmax_t maximum_file_bytes) {
    struct stat info{};
    if (::lstat(root.c_str(), &info) != 0) {
        if (errno == ENOENT)
            throw ServiceError(409, "candidate-artifact-missing", "Candidate artifact is missing.");
        invalid_artifact();
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0)
        invalid_artifact();
    const auto identity = route_directory_identity(root);
    const std::set<std::string> directories{"docs", "docs/kb", "docs/kb/generated"};
    std::set<std::string> files;
    // Validate ownership/types before reading any manifest bytes to select a format.
    for (const bool compact : {false, true})
        for (const auto& path : shin::context::expected_generated_relative_files(compact)) files.insert(path);
    std::set<std::string> actual_files;
    files.insert("docs/kb/.shin-context.lock");
    files.insert("docs/kb/.shin-context-writer.lock");
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const auto relative = entry.path().lexically_relative(root).generic_string();
        if (++count > directories.size() + files.size() || ::lstat(entry.path().c_str(), &info) != 0 ||
            info.st_uid != ::geteuid() || (info.st_mode & 0022) != 0) invalid_artifact();
        if (directories.contains(relative)) {
            if (!S_ISDIR(info.st_mode)) invalid_artifact();
        } else if (files.contains(relative)) {
            actual_files.insert(relative);
            if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 0 ||
                static_cast<std::uintmax_t>(info.st_size) > maximum_file_bytes) invalid_artifact();
        } else {
            invalid_artifact();
        }
    }
    shin::context::RuntimeOptions layout;
    layout.out_root = root;
    std::set<std::string> expected_files;
    for (const auto& path : shin::context::expected_generated_relative_files(layout)) expected_files.insert(path);
    expected_files.insert("docs/kb/.shin-context.lock");
    expected_files.insert("docs/kb/.shin-context-writer.lock");
    if (actual_files != expected_files || count != directories.size() + expected_files.size()) invalid_artifact();
    return identity;
}

} // namespace shin::server
