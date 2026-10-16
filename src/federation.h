#pragma once

#include "context_types.h"

namespace shin::context {

struct FederationQueryOptions {
    std::string query;
    int limit = 6;
    int per_doc_limit = 2;
    bool verify_current = false;
    bool require_current = false;
};

class FederationSession {
public:
    explicit FederationSession(fs::path config_path);
    ~FederationSession();

    FederationSession(const FederationSession&) = delete;
    FederationSession& operator=(const FederationSession&) = delete;
    FederationSession(FederationSession&&) noexcept;
    FederationSession& operator=(FederationSession&&) noexcept;

    const std::string& id() const noexcept;
    const fs::path& config_path() const noexcept;
    const std::string& config_hash() const noexcept;
    const std::vector<std::string>& member_ids() const noexcept;
    Json brief(const FederationQueryOptions& options) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace shin::context
