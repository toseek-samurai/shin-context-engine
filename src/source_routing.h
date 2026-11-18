#pragma once

#include "context_types.h"

namespace shin::context {
inline constexpr std::size_t kSourceIndexBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kSourceIndexRows = 65536;
inline constexpr std::string_view kSourceRoutingFile = "docs/kb/generated/source-routing.jsonl";
bool exceeds_eager_source_work(const SourceFactPathResult& paths);
bool needs_source_file_routing(const SourceFactPathResult& paths);
std::string source_search_projection(std::string_view text);
std::string source_routing_terms(const std::string& path, const std::string& content, std::size_t& chunks);
} // namespace shin::context
