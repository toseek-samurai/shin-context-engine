#pragma once

#include "context_types.h"

namespace shin::context {

struct WorktreeSourceState {
    std::string path;
    std::string status;
    std::string content_hash;
    std::uintmax_t byte_count = 0;
    FileMetadata metadata;
    bool has_metadata = false;
    bool operator==(const WorktreeSourceState&) const = default;
};

struct WorktreeInventory {
    std::map<std::string, WorktreeSourceState> documents;
    std::map<std::string, WorktreeSourceState> source_facts;
    ProjectIgnoreBinding exclusions;
    int source_fact_candidate_files = 0;
    int source_fact_capped_files = 0;
};

// Canonically framed, process-local metadata. No source bodies or content hashes.
// An empty payload means the optional checkpoint could not be represented.
struct WorktreeMetadataObservation {
    std::string payload;
    std::vector<std::string> fact_paths;
};

struct DoctorSourceAudit {
    struct Finding {
        std::string path, content_hash, rule;
    };
    int excluded_files = 0;
    std::size_t scanned_files = 0;
    std::vector<Finding> findings;
};

WorktreeInventory observe_worktree(const RuntimeOptions& options, DoctorSourceAudit* audit = nullptr);
void require_worktree_stable(const RuntimeOptions& options, const WorktreeInventory& inventory);
void require_worktree_metadata_stable(const RuntimeOptions& options, const WorktreeInventory& inventory);
std::string worktree_metadata_payload(const WorktreeInventory& inventory);
WorktreeMetadataObservation observe_worktree_metadata(const RuntimeOptions& options);
Json worktree_identity(const RuntimeOptions& options, const WorktreeInventory& inventory);
bool worktree_observation_complete(const WorktreeInventory& inventory);
Json compare_worktree(const RuntimeOptions& options, const Json& manifest,
                      const WorktreeInventory& inventory, int detail_limit);

}  // namespace shin::context
