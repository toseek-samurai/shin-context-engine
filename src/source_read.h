#pragma once

#include "context_types.h"

namespace shin::context {

std::string read_bound_source(const fs::path& root, const std::string& path,
                              const std::string& expected_hash, std::size_t maximum_bytes);
std::vector<std::size_t> source_line_offsets(const std::string& content);
std::string source_physical_range(const std::string& content,
                                  const std::vector<std::size_t>& offsets, int start, int end);
SecretScanHit source_range_secret(const std::string& text);

}  // namespace shin::context
