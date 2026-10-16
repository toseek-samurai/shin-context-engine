#pragma once

#include "candidate_artifact.h"
#include "route_catalog.h"
#include "snapshot_cache.h"
#include "store.h"
#include <optional>

namespace shin::server {

// Holds candidate ownership through a read operation; neither warm residency nor
// cached health can replace this admission and final verification.
class ActiveSnapshot {
public:
    ActiveSnapshot(const Config& config, const RouteCatalog& catalog, SnapshotCache& cache,
                   Store& store, const Principal& principal, const std::string& project,
                   const std::string& name, bool publication, const Json& active,
                   const std::string& state_identity, const std::function<Json()>& select,
                   const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
    void verify() const;
    const BoundRoute& binding() const { return binding_; }
    const SnapshotCache::Snapshot& snapshot() const { return snapshot_; }
private:
    Json read_workspace() const;
    const RouteCatalog& catalog_;
    const BoundRoute& binding_;
    Store& store_;
    Principal principal_;
    std::optional<WorkspaceReadRequest> scope_;
    std::optional<std::string> publication_;
    Json selected_;
    Json admission_;
    Json workspace_;
    std::unique_ptr<CandidateArtifactLock> lifetime_;
    SnapshotCache::Snapshot snapshot_;
};

} // namespace shin::server
