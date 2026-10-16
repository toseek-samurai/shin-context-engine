#pragma once

#include "retrieval_service.h"

namespace shin::server {

class ContinuityService {
public:
    ContinuityService(Store& store, RetrievalService& retrieval) : store_(store), retrieval_(retrieval) {}
    // Optional coordinates are native-internal inputs, never bound by legacy HTTP.
    Json save(const Principal& principal, const std::string& project, const std::string& slot, const Json& request,
              const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
    Json resume(const Principal& principal, const std::string& project, const std::string& slot,
                const Json& request, bool impact = false);
    // Personal HTTP permits active-route selectors only; internal historical reads may use publications.
    Json resume_workspace(const Principal& principal, const std::string& project, const std::string& slot,
                          const WorkspaceReadRequest& scope, const Json& request, bool impact = false, bool route_only = false);
    Json clear(const Principal& principal, const std::string& project, const std::string& slot, const Json& request,
               const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
    Json import_receipts(const Principal& principal, const std::string& project, const std::string& slot, const Json& request,
                         const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
private:
    Json resume_selected(const Principal& principal, const std::string& project, const std::string& slot,
                         const Json& request, bool impact, const std::optional<WorkspaceReadRequest>& scope, bool route_only = false);
    Store& store_;
    RetrievalService& retrieval_;
};

} // namespace shin::server
