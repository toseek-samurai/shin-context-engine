#pragma once

#include "context_types.h"
#include "worktree_state.h"

namespace shin::context {

struct VerifiedSnapshot;

// An observation is not a certificate. The caller may persist its fingerprint
// only around a successful full qualification of the same unchanged generation.
std::string generation_fingerprint(const RuntimeOptions& options);

// Request-scoped admission from a trusted, kit/policy-bound qualification receipt.
// No CLI flag imports receipts. The caller holds the publication lock throughout
// use and performs verify_content before returning or persisting a qualification.
class GenerationAdmission {
public:
    // Optional observations come from a fresh verified load under the same
    // publication lock, never a persisted metadata cache. They only deduplicate
    // output hashing; source observation and final verify_content remain required.
    static std::shared_ptr<const GenerationAdmission> reuse(
        const RuntimeOptions& options, const std::string& qualified_fingerprint,
        const VerifiedSnapshot* observed_snapshot = nullptr);
    // Transfer an already qualified generation to an identical published copy.
    // Caller holds both publication locks and independently authorizes the target.
    std::shared_ptr<const GenerationAdmission> published_copy(
        const RuntimeOptions& source, const RuntimeOptions& destination) const;
    // Verify two independently admitted copies in one byte pass. Both complete
    // closures remain fenced before/after; only identical physical paths with
    // identical expectations share a read. Caller holds both publication locks.
    std::size_t verify_content_together(const RuntimeOptions& options,
        const GenerationAdmission& other, const RuntimeOptions& other_options) const;
    IndexFreshness assess(const RuntimeOptions& options) const;
    void verify_content(const RuntimeOptions& options) const;
    std::uintmax_t reused_output_bytes() const noexcept { return reused_output_bytes_; }
    std::size_t reused_output_files() const noexcept { return reused_output_files_; }
    const std::string& fingerprint() const noexcept { return fingerprint_; }

private:
    explicit GenerationAdmission(const RuntimeOptions& options, const VerifiedSnapshot* observed_snapshot = nullptr);
    void verify_unchanged(const RuntimeOptions& options) const;
    void verify_binding(const RuntimeOptions& options) const;
    void verify_sources(const RuntimeOptions& options) const;
    void verify_outputs(const RuntimeOptions& options) const;
    void append_content_files(std::map<fs::path, FileFingerprint>& files) const;
    fs::path root_, out_root_;
    std::string profile_hash_, fingerprint_, portable_fingerprint_, snapshot_;
    WorktreeInventory sources_;
    std::map<std::string, FileFingerprint> outputs_;
    std::map<std::string, bool> links_;
    Json retool_environment_;
    bool retool_ = false;
    std::uintmax_t reused_output_bytes_ = 0;
    std::size_t reused_output_files_ = 0;
    friend std::string generation_fingerprint(const RuntimeOptions&);
};

} // namespace shin::context
