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
    int source_fact_candidate_files = 0;
    int source_fact_capped_files = 0;
};

WorktreeInventory observe_worktree(const RuntimeOptions& options);
void require_worktree_stable(const RuntimeOptions& options, const WorktreeInventory& inventory);
Json worktree_identity(const RuntimeOptions& options, const WorktreeInventory& inventory);
bool worktree_observation_complete(const WorktreeInventory& inventory);
Json compare_worktree(const RuntimeOptions& options, const Json& manifest,
                      const WorktreeInventory& inventory, int detail_limit);

}  // namespace shin::context
