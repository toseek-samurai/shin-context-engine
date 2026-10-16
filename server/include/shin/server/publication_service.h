#pragma once

#include "preparation_service.h"
#include <shared_mutex>

namespace shin::server {
class PublicationService {
public:
    PublicationService(const Config& config, const RouteCatalog& catalog, Store& store,
                       PreparationService& preparations);
    Json head(const Principal& principal, const std::string& project, const std::string& publication);
    Json submit(const Principal& principal, const std::string& project, const std::string& publication,
                const Json& request, const std::string& key, const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
    Json detail(const Principal& principal, const std::string& project, const std::string& publication, std::uint64_t id);
    Json list(const Principal& principal, const std::string& project, const std::string& publication,
              std::uint64_t after, std::size_t limit);
    Json resolve(const Principal& principal, const std::string& project, const std::string& publication,
                 std::uint64_t id, const Json& request, const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
    Json withdraw(const Principal& principal, const std::string& project, const std::string& publication,
                  const Json& request, const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
    Json review(const Principal& principal, const std::string& project, const std::string& publication, std::uint64_t id);
    Json retire_history(const Principal& principal, const Json& request,
                        const std::optional<WorkspaceWriteScope>& write = std::nullopt);

private:
    const Config& config_;
    const RouteCatalog& catalog_;
    Store& store_;
    PreparationService& preparations_;
    std::string state_identity_;
    std::shared_mutex review_mutex_;
};
} // namespace shin::server
