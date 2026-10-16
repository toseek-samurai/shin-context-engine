#pragma once

#include "io.h"
#include <cstdint>
#include <functional>

namespace shin::context { class GenerationAdmission; }

namespace shin::observer {
struct DeliveryConfig;
struct Config {
    Json binding;
    std::string binding_key;
    fs::path root, out_dir, profile_dir, state_dir;
    std::string profile;
    std::uint64_t poll_interval_ms = 1000;
    std::uintmax_t max_snapshot_bytes = 0;
    std::uintmax_t max_file_bytes = 0;
    bool outbox_enabled = false;
    std::uint64_t outbox_max_entries = 0;
    std::uint64_t outbox_max_bytes = 0;
};

Config load_config(const fs::path& path);
Config config_from_json(const Json& value, const fs::path& path);
// source_only is internal repair preparation, never query/readiness admission.
Json sample_worktree(const Config& config, const std::string& root_identity, bool source_only = false);
Json run_observer(const Config& config, std::uint64_t samples, const std::function<bool()>& stopped,
                  const DeliveryConfig* delivery = nullptr);
Json observer_status(const Config& config);
Json observer_metrics(const Config& config);
Json query_source(const Config& config, const std::string& query, bool materialize = false,
                  const fs::path& private_cache = {},
                  std::shared_ptr<const shin::context::GenerationAdmission> admission = {});
Json inspect_source(const Config& config, const std::string& path, const std::string& snapshot,
                    std::shared_ptr<const shin::context::GenerationAdmission> admission = {});
Json read_source(const Config& config, const std::string& request,
                 std::shared_ptr<const shin::context::GenerationAdmission> admission = {});
std::string source_view(const Json& result, std::size_t maximum_bytes = 8192);
Json archive_outbox(const Config& config, const fs::path& archive, bool verify_only);
void verify_observation(const Json& observation);
Json response(const Json& result);
std::int64_t now_ms();
std::string digest(const std::string& text);
} // namespace shin::observer
