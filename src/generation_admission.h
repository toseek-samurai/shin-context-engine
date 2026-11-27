#pragma once

#include "context_types.h"
#include "worktree_state.h"

namespace shin::context {

struct VerifiedSnapshot;

// A process-local checkpoint for idle reconciliation under uninterrupted native
// observation. It carries neither source bytes nor authority to serve a reader.
class IdleGenerationCheckpoint {
public:
    using Digest = std::string (*)(const std::string&);
    void verify(const RuntimeOptions& options) const;
    std::size_t retained_bytes() const noexcept;

private:
    explicit IdleGenerationCheckpoint(Digest digest) : digest_(digest) {}
    Digest digest_;
    fs::path root_, out_root_;
    std::string profile_hash_, source_metadata_, retool_environment_;
    std::vector<std::pair<std::string, FileMetadata>> outputs_;
    std::vector<std::pair<std::string, bool>> links_;
    friend class GenerationAdmission;
};

// An observation is not a certificate. The caller may persist its fingerprint
// only around a successful full qualification of the same unchanged generation.
std::string generation_fingerprint(const RuntimeOptions& options);

// Process-owned admission from a trusted, kit/policy-bound qualification receipt.
// No CLI flag imports receipts. The caller holds the publication lock throughout
// use and performs verify_content before returning or persisting a qualification.
// Observer may retain an admission in memory for background continuity only when
// healthy native events fence the full source/output closure. assess then checks
// inventory and metadata; it is not a new content audit or source-read authority.
class GenerationAdmission {
public:
    // Optional observations come from a fresh verified load under the same
    // publication lock, never a persisted metadata cache. They only deduplicate
    // output hashing; source observation and final verify_content remain required.
    // A process-local native observation may deduplicate intermediate membership
    // walks. It never skips live file metadata or the final full byte/inventory
    // fences; false/unavailable observation restores ordinary complete walks.
    static std::shared_ptr<const GenerationAdmission> reuse(
        const RuntimeOptions& options, const std::string& qualified_fingerprint,
        const VerifiedSnapshot* observed_snapshot = nullptr,
        std::function<bool()> unchanged_source_membership = {}, bool audit_automatic_sources = false);
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
    std::unique_ptr<const IdleGenerationCheckpoint> idle_checkpoint(IdleGenerationCheckpoint::Digest digest) const;
    const DoctorSourceAudit* doctor_source_audit(const RuntimeOptions& options) const;
    std::uintmax_t reused_output_bytes() const noexcept { return reused_output_bytes_; }
    std::size_t reused_output_files() const noexcept { return reused_output_files_; }
    const std::string& fingerprint() const noexcept { return fingerprint_; }

private:
    explicit GenerationAdmission(const RuntimeOptions& options, const VerifiedSnapshot* observed_snapshot = nullptr,
                                 bool audit_automatic_sources = false);
    void verify_unchanged(const RuntimeOptions& options, bool complete_inventory = false) const;
    void verify_binding(const RuntimeOptions& options) const;
    void verify_sources(const RuntimeOptions& options, bool complete_inventory = false) const;
    void verify_outputs(const RuntimeOptions& options) const;
    void append_content_files(std::map<fs::path, FileFingerprint>& files) const;
    fs::path root_, out_root_;
    std::string profile_hash_, fingerprint_, portable_fingerprint_, snapshot_;
    std::optional<DoctorSourceAudit> source_audit_;
    WorktreeInventory sources_;
    std::map<std::string, FileFingerprint> outputs_;
    std::map<std::string, bool> links_;
    Json retool_environment_;
    bool retool_ = false;
    // Process-local native observation can deduplicate intermediate membership
    // walks only. Metadata remains live; construction and final content fences
    // always enumerate the complete inventory. Never imported from state/RPC.
    std::function<bool()> unchanged_source_membership_;
    std::uintmax_t reused_output_bytes_ = 0;
    std::size_t reused_output_files_ = 0;
    friend std::string generation_fingerprint(const RuntimeOptions&);
};

} // namespace shin::context
