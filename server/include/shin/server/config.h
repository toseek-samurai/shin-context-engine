#pragma once

#include "model.h"
#include "oidc_provider.h"
#include <optional>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace shin::server {

inline constexpr char kServiceConfigSchema[] = "shin.server-config.v10";
inline constexpr char kRecoveryConfigSchema[] = "shin.server-recovery-config.v1";

using Digest = std::array<unsigned char, 32>;

struct Credential {
    Principal principal;
    Digest token_hash;
};

struct RetrievalRoute {
    std::string id;
    std::string project_id;
    std::string workspace_id;
    std::string source_host_id;
    std::string root;
    std::string root_identity;
    std::string profile;
    std::filesystem::path profile_dir;
};

struct RetrievalConfig {
    std::vector<RetrievalRoute> routes;
    std::uintmax_t max_payload_bytes = 512ULL * 1024 * 1024;
    std::uintmax_t max_generation_bytes = 128ULL * 1024 * 1024;
    std::uintmax_t max_file_bytes = 64ULL * 1024 * 1024;
    std::size_t max_generations = 8;
};

struct PreparationConfig {
    bool enabled = false;
    std::filesystem::path state_dir;
    std::size_t max_retained_jobs = 8;
    int lease_seconds = 30;
};

struct ObservabilityConfig {
    std::size_t max_retained_requests = 128;
    std::size_t max_query_bytes = 4096;
    std::vector<std::string> capture_query_projects;
};

struct KnowledgeConfig {
    bool enabled = false;
    std::string principal_id;
    std::vector<std::string> route_ids;
    int interval_seconds = 60;
    int max_age_seconds = 180;
    bool prepare_on_drift = false;
};

struct JournalWriterConfig {
    bool enabled = false;
    std::string principal_id;
    int interval_ms = 1000;
    int max_retry_ms = 30000;
    bool flush_on_shutdown = true;
};

struct Config {
    std::string host;
    int port = 0;
    StoreConfig database;
    std::vector<std::filesystem::path> allowed_roots;
    std::vector<Credential> credentials;
    std::optional<OidcProviderOptions> oidc;
    RetrievalConfig retrieval;
    PreparationConfig preparations;
    ObservabilityConfig observability;
    KnowledgeConfig knowledge;
    JournalWriterConfig journal_writer;
    std::size_t workers = 4;
    std::size_t queue_limit = 64;
};

Json parse_bounded_json(std::string_view text, std::size_t maximum_bytes);
Json load_private_json(const std::filesystem::path& path, std::size_t maximum_bytes);
void require_fields(const Json& value, std::initializer_list<std::string_view> fields);
std::string require_text(const Json& value, std::string_view field, std::size_t maximum_bytes);
bool valid_identifier(std::string_view value);
Digest sha256(std::string_view input);
std::string digest_hex(const Digest& digest);
Config load_config(const std::filesystem::path& path);
StoreConfig load_recovery_config(const std::filesystem::path& path);
const Principal& authenticate(const Config& config, std::string_view token);
ProjectInput project_input(const Config& config, const Json& value);
WorkspaceInput workspace_input(const Config& config, const Json& value);

} // namespace shin::server
