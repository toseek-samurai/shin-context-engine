#pragma once

#include "context_types.h"
#include "worktree_state.h"
#include <mutex>

namespace shin::context {

// One validate invocation may share the expected output of an independent
// generation. This is neither a freshness verdict nor a quality/publication
// certificate: every gate still inspects actual outputs and runs its checks.
class ValidationGeneration {
public:
    explicit ValidationGeneration(const RuntimeOptions& options,
        std::uintmax_t maximum_bytes = 256ULL * 1024 * 1024);
    std::shared_ptr<const GeneratedOutput> acquire(const RuntimeOptions& options, bool compact);
    Json metrics() const;
private:
    void verify_inputs(const RuntimeOptions& options);
    void verify_binding(const RuntimeOptions& options) const;
    mutable std::mutex mutex_;
    fs::path root_, out_root_, profile_dir_;
    std::string profile_hash_;
    Profile profile_;
    std::uintmax_t maximum_bytes_;
    WorktreeInventory sources_;
    std::map<std::string, bool> links_;
    std::vector<std::string> fact_paths_;
    Json retool_environment_;
    bool retool_ = false, compact_ = true, disabled_ = false;
    std::shared_ptr<const GeneratedOutput> generated_;
    std::uint64_t reuses_ = 0, invalidations_ = 0, inventory_checks_ = 0, hash_files_ = 0, hash_bytes_ = 0;
    double verification_ms_ = 0;
    std::string fallback_reason_;
};

std::shared_ptr<const GeneratedOutput> validation_generated_output(
    const RuntimeOptions& options, bool compact = true);

} // namespace shin::context
