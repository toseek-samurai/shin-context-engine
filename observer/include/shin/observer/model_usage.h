#pragma once
#include "shin/observer/io.h"

namespace shin::observer {
Json summarize_model_usage(const Json& input, std::int64_t now);
Json read_model_usage(const fs::path& path, std::int64_t now);
Json collect_model_usage(const Json& config, std::int64_t now);
} // namespace shin::observer
