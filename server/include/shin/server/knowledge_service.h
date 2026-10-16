#pragma once

#include "knowledge_scheduler.h"
#include "preparation_service.h"
#include "retrieval_service.h"

namespace shin::server {

class KnowledgeService {
public:
    KnowledgeService(const Config& config, const RouteCatalog& catalog, Store& store,
                     RetrievalService& retrieval, PreparationService& preparations);
    void start() { scheduler_.start(); }
    void request_stop() { scheduler_.request_stop(); }
    Json status(const Principal& principal) const;
    Json refresh(const Principal& principal, const Json& request);
    Json health(const Principal& principal, const std::string& project, const std::string& route);
    Json routes(const Principal& principal, const std::string& project);
    Json bootstrap(const Principal& principal, const Json& request);
private:
    Json inspect(const KnowledgeTarget& target);
    Json publication_health(const Principal& principal, const std::string& project, const std::string& publication);
    Json selected_health(const Principal& principal, const std::string& project, const std::string& route,
                         const Json& active, bool publication);
    const Config& config_;
    const RouteCatalog& catalog_;
    Store& store_;
    RetrievalService& retrieval_;
    PreparationService& preparations_;
    Principal actor_;
    KnowledgeScheduler scheduler_;
};

} // namespace shin::server
