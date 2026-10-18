#pragma once
#include "shin/observer/io.h"
#include <stop_token>

namespace shin::observer {
Json summarize_model_usage(const Json& input, std::int64_t now);
Json read_model_usage(const fs::path& path, std::int64_t now);
Json collect_model_usage(const Json& config, std::int64_t now, std::stop_token stop = {});
// Local numerical history only. No network requests or client settings writes.
Json default_model_usage_sources(char** environment);
Json collect_registered_model_usage(const fs::path& home, const Json& sources,
                                   const Json& seed = nullptr, std::stop_token stop = {});
void watch_registered_model_usage(const fs::path& home, const Json& sources, std::stop_token stop);
} // namespace shin::observer
